#!/usr/bin/env python3
"""Proves the whole mouth path: audio in, the right drawn shape on screen.

Every layer below this is already tested on its own -- the classifier against synthesised vowels,
the band layout, the hold timing. None of that shows that the number reaches the shader or that
the shader picks the cell the number names. A mouth that is always shape A looks perfectly
plausible in a screenshot and passes every "does it render" check ever written.

So the placeholder strip gives each shape a DIFFERENT HUE, and this plays vowels synthesised at
published formants and reads the hue back off the frame. The hue says which cell was drawn, which
is the one thing no other test here can see.

ARMED by mutating the SHADER, which is the half nothing else covers:

    control (everything wired up)             5/5 passed
    mouth stuck on the first cell             1/5   -- only "silence draws A" survives, which is
                                                       exactly the false pass this file is for
    rest folds to the wrong shape             4/5

    tools/mouth-proof.py --plugin-build . --pack ../foxfire/packs/mouth
"""

import argparse
import asyncio
import base64
import colorsys
import io
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ff_proof  # noqa: E402
import proof  # noqa: E402

from PIL import Image  # noqa: E402

W, H = 640, 360
SR = 48000
FAILS: list[str] = []
CHECKS: list[str] = []

# Hue of each placeholder cell, and the shape it stands for. From the strip generator.
CELL_HUE = {"A": 0, "B": 36, "C": 64, "D": 120, "E": 203, "F": 272}


def check(name, ok, detail):
    CHECKS.append(name)
    if not ok:
        FAILS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def synth_vowel(path: Path, f1: float, f2: float, f3: float, secs: float = 6.0, f0: float = 120.0):
    """A vowel: glottal pulses through three formant resonators, then normalised.

    The same source-filter model tests/test_viseme.c uses, for the same reason -- the right answer
    is known by construction. Normalising is not optional: cascaded resonators have gain that
    depends on the formants, so without it each vowel arrives at a different level and clipped.
    """
    n = int(SR * secs)
    y1 = [0.0, 0.0, 0.0]
    y2 = [0.0, 0.0, 0.0]
    a1, a2 = [], []
    for hz, bw in ((f1, 80.0), (f2, 100.0), (f3, 150.0)):
        r = math.exp(-math.pi * bw / SR)
        a1.append(2.0 * r * math.cos(2.0 * math.pi * hz / SR))
        a2.append(-r * r)
    out = [0.0] * n
    phase, step = 0.0, f0 / SR
    for i in range(n):
        phase += step
        x = 0.0
        if phase >= 1.0:
            phase -= 1.0
            x = 1.0
        for k in range(3):
            y = x + a1[k] * y1[k] + a2[k] * y2[k]
            y2[k] = y1[k]
            y1[k] = y
            x = y
        out[i] = x
    mx = max(abs(v) for v in out) or 1.0
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(v / mx * 0.3 * 32000)) for v in out))


async def shoot(c, name):
    r = await c.request("GetSourceScreenshot", {"sourceName": name, "imageFormat": "png",
                                                "imageWidth": W, "imageHeight": H})
    return Image.open(io.BytesIO(base64.b64decode(r["imageData"].split(",", 1)[1]))).convert("RGBA")


def dominant_hue(img: Image.Image):
    """Hue of the drawn pixels, ignoring anything transparent or near-black.

    The cell's opening is painted near-black on purpose, so it has to be excluded or every shape
    averages toward the same dark grey and the hue stops identifying anything.
    """
    hues, weight = [], 0.0
    for r, g, b, a in img.getdata():
        if a < 200:
            continue
        mx, mn = max(r, g, b), min(r, g, b)
        if mx < 80 or mx - mn < 40:   # too dark or too grey to carry a hue
            continue
        h, _, _ = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        hues.append(h * 360.0)
        weight += 1.0
    if not hues:
        return None, 0
    # circular mean, because hue wraps and A sits at 0
    x = sum(math.cos(math.radians(h)) for h in hues)
    y = sum(math.sin(math.radians(h)) for h in hues)
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0, int(weight)


def nearest_shape(hue):
    if hue is None:
        return None
    best, bd = None, 1e9
    for name, h in CELL_HUE.items():
        d = abs((hue - h + 180) % 360 - 180)
        if d < bd:
            best, bd = name, d
    return best


async def drive(pack_id: str, wavs: dict, scratch: Path):
    ws, c = await ff_proof.open_client("mouth proof")
    seen = {}
    try:
        for _ in range(60):
            try:
                await c.request("GetVersion")
                break
            except Exception:
                await asyncio.sleep(1)
        await c.request("CreateScene", {"sceneName": "mo"})
        await c.request("SetCurrentProgramScene", {"sceneName": "mo"})
        await c.request("CreateInput", {
            "sceneName": "mo", "inputName": "voice", "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": str(wavs["ee"]),
                              "looping": True}})
        await asyncio.sleep(1.5)
        await c.request("CreateInput", {
            "sceneName": "mo", "inputName": "mouth", "inputKind": "foxfire_visualizer",
            "inputSettings": {"pack": pack_id, "preset": "mouth-strip", "width": W, "height": H,
                              "audio_mode": 1, "audio_source": "voice"}})
        await asyncio.sleep(2.0)

        # Silence first: nothing playing, so the mouth must be at rest -- which folds to A.
        await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
            "local_file": str(wavs["silence"])}})
        await c.request("TriggerMediaInputAction", {
            "inputName": "voice", "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
        await asyncio.sleep(3.0)
        hue, px = dominant_hue(await shoot(c, "mouth"))
        seen["silence"] = (nearest_shape(hue), hue, px)

        for vowel in ("ee", "ah", "oo"):
            await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
                "local_file": str(wavs[vowel])}})
            await c.request("TriggerMediaInputAction", {
                "inputName": "voice", "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
            await asyncio.sleep(3.0)
            hue, px = dominant_hue(await shoot(c, "mouth"))
            seen[vowel] = (nearest_shape(hue), hue, px)
    finally:
        await ws.close()
    return seen


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    ap.add_argument("--pack", required=True)
    args = ap.parse_args()

    repo = Path(args.plugin_build).resolve()
    pack_dir = Path(args.pack).resolve()
    pack_id = json.loads((pack_dir / "pack.json").read_text())["id"]

    cfg = Path(tempfile.mkdtemp(prefix="ff-mouth-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    scratch = Path(tempfile.mkdtemp(prefix="ff-mouth-src-"))
    try:
        wavs = {}
        # the same formants tests/test_viseme.c uses, and the shape each one should draw
        for name, (f1, f2, f3) in {"ee": (240, 2400, 2900), "ah": (850, 1610, 2600),
                                   "oo": (250, 595, 2400)}.items():
            wavs[name] = scratch / f"{name}.wav"
            synth_vowel(wavs[name], f1, f2, f3)
        wavs["silence"] = scratch / "silence.wav"
        with wave.open(str(wavs["silence"]), "w") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
            w.writeframes(b"\0\0" * (SR * 6))

        proof.write_ws_config(obs_cfg)
        proof.install_plugin(repo, obs_cfg)
        proof.install_pack(pack_dir, obs_cfg)
        proof.wait_for_port_free(proof.PORT)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi",
             "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            seen = asyncio.run(drive(pack_id, wavs, scratch))
        finally:
            proof.terminate_process_group(p)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    want = {"silence": "A", "ee": "B", "ah": "D", "oo": "F"}
    for key, expect in want.items():
        got, hue, px = seen.get(key, (None, None, 0))
        check(f"{key} draws shape {expect}", got == expect,
              f"hue {hue if hue is None else round(hue)} over {px} lit pixels -> {got}")

    # And they are not all the same cell, which a mouth stuck on one shape would also satisfy
    # check by check above if the expectations happened to agree.
    shapes = {k: v[0] for k, v in seen.items()}
    check("the shape actually changes with the audio", len(set(shapes.values())) >= 3,
          f"{shapes} -- a mouth stuck on one shape renders perfectly well and is the failure "
          f"this whole file exists to catch")

    print(f"\nmouth proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("mouth proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
