#!/usr/bin/env python3
"""Proves the whole mouth path: audio in, the right drawn shape on screen.

Every layer below this is already tested on its own -- the classifier against synthesised vowels,
the band layout, the hold timing. None of that shows that the number reaches the shader or that
the shader picks the cell the number names. A mouth that is always shape A looks perfectly
plausible in a screenshot and passes every "does it render" check ever written.

So this generates a strip of its own -- one flat hue per cell -- binds it over the pack's art,
plays vowels synthesised at published formants and reads the hue back off the frame. The hue says
which cell was drawn, which is the one thing no other test here can see.

The strip is generated rather than read from the pack because the pack's art is a PLACEHOLDER
waiting to be replaced by a drawing, and in a drawing the hue is the same in every cell -- it is
one mouth in six positions. Keyed to the placeholder, this whole file would have failed the day
the pack became real. Verified by doing it: with the pack's art swapped for a drawn-style strip
in one skin tone, where hue identifies nothing, this still scores 9/9.

It also checks the Mouth controls in the properties panel. Those are ENGINE state, not shader
uniforms, so no render can show whether the panel is wired to the engine or to nothing -- the
mouth looks equally correct either way. Each is checked by making it change a shape already
known from the sweep above.

WHAT THIS DOES NOT COVER, of the four controls: only "Silence threshold" and "Minimum shape
time" are here. "Closed-mouth gap" chooses between rest and a closure, and the placeholder strip
folds rest onto the closed cell, so both land on the same hue and no colour can tell them apart.
"Mouth close speed" moves `mouth_open`, which changes no cell index. Nor is the `uses_mouth()`
half covered -- obs-websocket cannot enumerate a source's properties, so whether the group
APPEARS is unproven here; what is proven is that the settings reach the classifier.

ARMED by mutation -- every number below was measured by running it, not predicted:

    control (everything wired up)             9/9 passed
    mouth stuck on the first cell             3/9   -- the survivors are "silence draws A", the
                                                       gate check (both want A anyway) and the
                                                       pixel count: exactly the false pass this
                                                       file is for
    rest folds to the wrong shape             7/9
    mouth settings never reach the engine     7/9
    hold wired to the release field           8/9
    frication branch removed                  8/9   -- the hiss goes back to drawing an open jaw
    uses_mouth() forced false                 9/9   -- UNARMED, as stated above
    pack art swapped for drawn-style art      9/9   -- the point: it must NOT fail

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

# The probe strip this file draws for itself: one flat hue per cell, so a screenshot says WHICH
# cell was selected. It is generated here and bound over the pack's own art through the
# user-image path, deliberately.
#
# Reading the hues off the PACK's art is what this used to do, and it made the gate die exactly
# when the pack became real: the placeholder is a stand-in for junkie's drawing, and the first
# thing that happens to it is being replaced. A proof that cannot survive its subject being
# finished is not a proof of the subject.
CELL_HUE = {"A": 0, "B": 36, "C": 64, "D": 120, "E": 203, "F": 272}
STRIP_CELL = 256


def check(name, ok, detail):
    CHECKS.append(name)
    if not ok:
        FAILS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def render_vowel(f1: float, f2: float, f3: float, secs: float = 6.0, f0: float = 120.0):
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
    return [v / mx * 0.3 for v in out]


def make_probe_strip(path: Path):
    """One flat, saturated cell per shape, in strip order."""
    order = ["A", "B", "C", "D", "E", "F"]
    img = Image.new("RGBA", (STRIP_CELL * len(order), STRIP_CELL), (0, 0, 0, 0))
    for i, name in enumerate(order):
        r, g, b = colorsys.hsv_to_rgb(CELL_HUE[name] / 360.0, 0.85, 0.95)
        cell = Image.new("RGBA", (STRIP_CELL, STRIP_CELL),
                         (int(r * 255), int(g * 255), int(b * 255), 255))
        img.paste(cell, (i * STRIP_CELL, 0))
    img.save(path)


def render_fricative(hz: float, bw: float, secs: float = 6.0):
    """Noise through one broad resonator, then lip radiation -- an "sss".

    The radiation term is the same one the vowel gets. Applying it to one and not the other
    would BE the difference the classifier is being asked to find.
    """
    n = int(SR * secs)
    r = math.exp(-math.pi * bw / SR)
    a1 = 2.0 * r * math.cos(2.0 * math.pi * hz / SR)
    a2 = -r * r
    y1 = y2 = prev = 0.0
    seed = 1
    out = [0.0] * n
    for i in range(n):
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        x = (seed >> 8) / 8388608.0 - 1.0
        y = x + a1 * y1 + a2 * y2
        y2, y1 = y1, y
        out[i] = y - prev
        prev = y
    mx = max(abs(v) for v in out) or 1.0
    return [v / mx * 0.3 for v in out]


def write_wav(path: Path, samples):
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(v * 32000)) for v in samples))


async def shoot(c, name):
    r = await c.request("GetSourceScreenshot", {"sourceName": name, "imageFormat": "png",
                                                "imageWidth": W, "imageHeight": H})
    return Image.open(io.BytesIO(base64.b64decode(r["imageData"].split(",", 1)[1]))).convert("RGBA")


def dominant_hue(img: Image.Image):
    """Hue of the drawn pixels, ignoring anything transparent or near-black.

    The probe strip is flat colour, so the filtering is not for its benefit -- it is what lets
    the same reading work on drawn art, where the opening of the mouth is near-black and the
    margins are transparent. Without it those average every shape toward the same dark grey.
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


async def drive(pack_id: str, wavs: dict, strip: Path):
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
                              "audio_mode": 1, "audio_source": "voice",
                              # the probe strip, over the pack's own art. "l0.mouth" is the
                              # renderer's key for layer 0's user-supplied image.
                              "l0.mouth": str(strip)}})
        await asyncio.sleep(2.0)

        # Silence first: nothing playing, so the mouth must be at rest -- which folds to A.
        await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
            "local_file": str(wavs["silence"])}})
        await c.request("TriggerMediaInputAction", {
            "inputName": "voice", "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
        await asyncio.sleep(3.0)
        hue, px = dominant_hue(await shoot(c, "mouth"))
        seen["silence"] = (nearest_shape(hue), hue, px)

        for vowel in ("ee", "ah", "oo", "hiss"):
            await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
                "local_file": str(wavs[vowel])}})
            await c.request("TriggerMediaInputAction", {
                "inputName": "voice", "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
            await asyncio.sleep(3.0)
            hue, px = dominant_hue(await shoot(c, "mouth"))
            seen[vowel] = (nearest_shape(hue), hue, px)

        async def set_mouth(key, value):
            await c.request("SetInputSettings",
                            {"inputName": "mouth", "inputSettings": {key: value}})

        async def rest_then(path, settle=3.0):
            """Silence long enough to reach rest, then this file from the top.

            The silence is not padding. Coming out of rest is deliberately immediate, so this
            is what makes the first voiced frame choose a shape rather than inherit whatever
            the previous clip left up.
            """
            for f in (wavs["silence"], path):
                await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
                    "local_file": str(f)}})
                await c.request("TriggerMediaInputAction", {
                    "inputName": "voice",
                    "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
                await asyncio.sleep(2.5 if f is wavs["silence"] else settle)
            return dominant_hue(await shoot(c, "mouth"))

        # GATE, at the top of its own slider range: the test tone sits near 0.08, so a mouth
        # that reads the setting stops answering the audio at all and falls back to rest.
        await set_mouth("mouth.gate", 0.3)
        hue, px = await rest_then(wavs["ah"])
        seen["gated"] = (nearest_shape(hue), hue, px)
        await set_mouth("mouth.gate", 0.04)

        # HOLD: one file, "ee" running straight into "oo". At the shipped hold the mouth has
        # moved to F by a second into the second vowel; held for eight seconds it is still on
        # B, the shape the FIRST vowel chose. Same audio both times -- only the setting differs.
        hue, px = await rest_then(wavs["ee_oo"], settle=4.5)
        seen["ee_oo_free"] = (nearest_shape(hue), hue, px)
        await set_mouth("mouth.hold_ms", 8000.0)
        hue, px = await rest_then(wavs["ee_oo"], settle=4.5)
        seen["ee_oo_held"] = (nearest_shape(hue), hue, px)
        await set_mouth("mouth.hold_ms", 80.0)

        # And the pack's OWN art, once, because nothing else here looks at it any more. Not by
        # colour -- that is the coupling this file just got rid of -- but it has to draw
        # something: "" means the pack's own image, and a strip that failed to load draws
        # nothing at all.
        await c.request("SetInputSettings",
                        {"inputName": "mouth", "inputSettings": {"l0.mouth": ""}})
        await rest_then(wavs["ah"])
        _, px = dominant_hue(await shoot(c, "mouth"))
        seen["pack_art"] = (None, None, px)
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
            write_wav(wavs[name], render_vowel(f1, f2, f3))

        # A hiss. Nothing about it is a vowel, and before the classifier looked for frication
        # it drew C -- a half-open jaw on every "s" in every sentence.
        wavs["hiss"] = scratch / "hiss.wav"
        write_wav(wavs["hiss"], render_fricative(5200.0, 3000.0))

        # "ee" running straight into "oo" with NO gap between them. The gap is the point: a
        # silence long enough to reach the mouth resets it to rest, and coming out of rest is
        # deliberately immediate, which bypasses the hold entirely. Switching files to change
        # vowel would therefore pass whether the hold works or not.
        wavs["ee_oo"] = scratch / "ee_oo.wav"
        write_wav(wavs["ee_oo"],
                  render_vowel(240, 2400, 2900, secs=3.0) + render_vowel(250, 595, 2400, secs=3.0))
        strip = scratch / "probe-strip.png"
        make_probe_strip(strip)
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
            seen = asyncio.run(drive(pack_id, wavs, strip))
        finally:
            proof.terminate_process_group(p)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    want = {"silence": "A", "ee": "B", "ah": "D", "oo": "F", "hiss": "B"}
    for key, expect in want.items():
        got, hue, px = seen.get(key, (None, None, 0))
        check(f"{key} draws shape {expect}", got == expect,
              f"hue {hue if hue is None else round(hue)} over {px} lit pixels -> {got}")

    # And they are not all the same cell, which a mouth stuck on one shape would also satisfy
    # check by check above if the expectations happened to agree.
    # pack_art excluded on purpose: it is a pixel-count check with no shape, and counting its
    # None as a distinct value would let a mouth stuck on one cell satisfy this.
    shapes = {k: v[0] for k, v in seen.items() if k != "pack_art"}
    check("the shape actually changes with the audio", len(set(shapes.values())) >= 3,
          f"{shapes} -- a mouth stuck on one shape renders perfectly well and is the failure "
          f"this whole file exists to catch")

    # The two Mouth controls whose effect is visible in WHICH CELL is drawn. Nothing else can
    # see these: they are engine state, not shader uniforms, so every render looks correct
    # whether the panel is wired to the engine or to nothing.
    got, hue, px = seen.get("gated", (None, None, 0))
    check("raising the silence threshold silences the mouth", got == "A",
          f"a tone at ~0.08 under a gate of 0.3 -> {got} (hue {hue if hue is None else round(hue)}); "
          f"D would mean the setting never reached the engine")
    check("the pack's own art draws", seen.get("pack_art", (None, None, 0))[2] > 500,
          f"{seen.get('pack_art', (None, None, 0))[2]} lit pixels with the shipped strip bound "
          f"-- every other check here uses a strip this file generates, so this is the only one "
          f"that touches the art the pack ships")
    free = seen.get("ee_oo_free", (None, None, 0))[0]
    held = seen.get("ee_oo_held", (None, None, 0))[0]
    check("the hold time keeps a shape up", free == "F" and held == "B",
          f"same audio, ee running into oo: shipped hold -> {free}, hold 8000 ms -> {held} "
          f"(want F then B)")

    print(f"\nmouth proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("mouth proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
