#!/usr/bin/env python3
"""Proves an alert actually draws its name and actually makes a sound.

These are the two things the layer renderer has never done, and the two the research could only
read out of headers (foxfire/research/alerts-rendering.md says so in its own Limits section). So
both are measured here through OBS, not inferred:

  1. idle draws NOTHING -- an alert overlay spends its life waiting, and one that tints the
     canvas while idle is one nobody can leave in a scene
  2. firing draws ink, and the ink goes away again when the alert is over
  3. the NAME is what is drawn -- changing the template changes the pixels, so this is the text
     path and not some other mark
  4. a name carrying a right-to-left override and a newline is sanitised before it is drawn
  5. the sound reaches OBS's own audio metering -- the composite audio_render path working, which
     is the piece that exists only because a private child source sits in no scene

Usage:
    tools/alert-proof.py --plugin-build build_x86_64
"""

import argparse
import asyncio
import base64
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ff_proof  # noqa: E402
import proof  # noqa: E402

from PIL import Image  # noqa: E402

W, H = 800, 240
FAILS: list[str] = []
CHECKS: list[str] = []


def check(name, ok, detail):
    CHECKS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILS.append(name)


def write_tone_wav(path: Path, seconds=3.0, hz=440.0, rate=48000):
    """A loud, plain tone. Loud on purpose: the check is "did ANY audio arrive", and a quiet clip
    would leave a pass/fail decision resting on a meter's noise floor."""
    frames = int(seconds * rate)
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        import math
        w.writeframes(b"".join(
            struct.pack("<h", int(28000 * math.sin(2 * math.pi * hz * i / rate)))
            for i in range(frames)))


async def shoot(c) -> Image.Image:
    r = await c.request("GetSourceScreenshot", {"sourceName": "alert", "imageFormat": "png",
                                                "imageWidth": W, "imageHeight": H})
    return Image.open(io.BytesIO(base64.b64decode(r["imageData"].split(",", 1)[1]))).convert("RGBA")


def ink(img: Image.Image) -> int:
    """Pixels with real alpha. Text is thin, so this counts rather than averaging -- a mean over
    an 800x240 frame barely moves for a line of 48px type."""
    return sum(1 for p in img.convert("RGBA").getchannel("A").point(
        [255 if i > 60 else 0 for i in range(256)]).getdata() if p)


def ink_width(img: Image.Image) -> int:
    """How wide the drawn text is. Independent of how much ink it happens to contain, so a longer
    message is distinguishable from a bolder one -- and, unlike a count, it reveals clipping
    (a bbox that reaches the canvas edge)."""
    bb = img.convert("RGBA").getchannel("A").point(
        [255 if i > 60 else 0 for i in range(256)]).getbbox()
    return 0 if not bb else bb[2] - bb[0]


async def wait_ready(c, timeout=60.0):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            await c.request("GetSceneList")
            return
        except Exception as e:  # noqa: BLE001 -- any failure here means "not ready yet"
            last = e
            await asyncio.sleep(1.0)
    raise RuntimeError(f"OBS never became ready: {last!r}")


async def fire(c):
    """The REAL trigger path: the properties panel's button, not a private back door."""
    await c.request("PressInputPropertiesButton", {"inputName": "alert", "propertyName": "test"})


async def drive(sound: Path, logdir: Path):
    ws, c = await ff_proof.open_client("alert proof")
    try:
        await wait_ready(c)
        await c.request("CreateScene", {"sceneName": "al"})
        await c.request("SetCurrentProgramScene", {"sceneName": "al"})
        await c.request("CreateInput", {
            "sceneName": "al", "inputName": "alert", "inputKind": "foxfire_alert",
            "inputSettings": {"width": W, "height": H, "duration": 4.0,
                              "template": "{name} followed!", "sound": str(sound),
                              "font_size": 64}})
        await asyncio.sleep(1.5)

        idle = ink(await shoot(c))
        check("an idle alert draws nothing at all", idle == 0,
              f"{idle} pixels with alpha>60 before anything fired")

        await fire(c)
        await asyncio.sleep(1.2)
        lit = await shoot(c)
        lit.save("/tmp/ff-alert-fired.png")  # kept for a human to look at; the checks are numeric
        n_lit = ink(lit)
        check("firing draws ink", n_lit > 200, f"{n_lit} pixels (idle was {idle})")

        # The name IS the ink: a different template must change the picture. Measured as the
        # WIDTH of the drawn text, not as a pixel count -- a longer message at 64px overflows an
        # 800px canvas, gets clipped to the same visible area, and lands on a nearly identical
        # count. The first version of this check did exactly that and read 21882 both times.
        # A settings change has to LAND before the button is pressed. Without this wait each
        # measurement came out one step stale -- the short message measured 698px (still at the
        # previous 64px size) and the long one 268px (still the short text, now at 24px) -- which
        # reads as "longer text draws narrower" and sent me looking for a bug in the shader side
        # of a feature that has no shader.
        await c.request("SetInputSettings", {"inputName": "alert", "inputSettings": {"font_size": 24}})
        await asyncio.sleep(0.8)
        await fire(c)
        await asyncio.sleep(1.2)
        w_short = ink_width(await shoot(c))
        await c.request("SetInputSettings", {"inputName": "alert",
                                             "inputSettings": {"template": "{name} followed and here is a much longer line"}})
        await asyncio.sleep(0.8)
        await fire(c)
        await asyncio.sleep(1.2)
        w_long = ink_width(await shoot(c))
        check("the template decides what is drawn, so this is the text path",
              w_short > 0 and w_long > w_short * 1.3 and w_long < W,
              f"drawn text is {w_short}px wide for the short message and {w_long}px for the long "
              f"one (both inside the {W}px canvas, so neither is clipped)")

        # back to the short one, then let it run out
        await c.request("SetInputSettings", {"inputName": "alert",
                                             "inputSettings": {"template": "{name} followed!",
                                                               "font_size": 64}})
        await asyncio.sleep(0.8)
        await fire(c)
        await asyncio.sleep(5.5)  # duration is 4.0
        n_after = ink(await shoot(c))
        check("the alert goes away when it is over", n_after == 0,
              f"{n_after} pixels 5.5s after firing a 4.0s alert")
    finally:
        await ws.close()
    return logdir


async def record_alert_audio(rec_dir: Path) -> tuple[Path, Path]:
    """Records OBS twice -- once idle, once with an alert firing -- and returns both files.

    A RECORDING, not a meter. obs-websocket gates its volume meters and GetInputAudioTracks on
    OBS_SOURCE_AUDIO, and libobs refuses that flag on a composite source outright ("Composite
    sources cannot be audio sources"), so this source is invisible to them by construction. The
    recording is the thing that matters anyway: it is what a viewer would hear.

    The idle pass is the control. Without it "there is audio in the file" proves nothing -- it
    could be anything else in the scene, or OBS's own silence padding measuring non-zero.
    """
    ws, c = await ff_proof.open_client("alert audio")
    try:
        await c.request("SetRecordDirectory", {"recordDirectory": str(rec_dir)})

        await c.request("StartRecord")
        await asyncio.sleep(3.0)                      # idle: nothing fired
        quiet = (await c.request("StopRecord"))["outputPath"]
        await asyncio.sleep(1.0)

        await c.request("StartRecord")
        await asyncio.sleep(0.5)
        await c.request("PressInputPropertiesButton",
                        {"inputName": "alert", "propertyName": "test"})
        await asyncio.sleep(3.0)
        loud = (await c.request("StopRecord"))["outputPath"]
        await asyncio.sleep(1.0)
        return Path(quiet), Path(loud)
    finally:
        await ws.close()


def audio_rms(path: Path) -> float:
    """RMS of a recording's audio, 0..1. Decoded to raw s16 mono so nothing depends on parsing
    ffmpeg's human-readable output, which changes between versions."""
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-vn", "-ac", "1", "-ar", "16000",
                        "-f", "s16le", "-"], capture_output=True)
    raw = r.stdout
    if not raw:
        raise RuntimeError(f"no audio decoded from {path}: {r.stderr.decode(errors='replace')[:300]}")
    import array
    a = array.array("h")
    a.frombytes(raw[: len(raw) // 2 * 2])
    if not len(a):
        return 0.0
    return (sum(float(v) * v for v in a) / len(a)) ** 0.5 / 32768.0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True, help="built engine repo (has install-local.sh)")
    args = ap.parse_args()

    repo = Path(args.plugin_build).resolve()
    scratch = Path(tempfile.mkdtemp(prefix="ff-alert-src-"))
    cfg = Path(tempfile.mkdtemp(prefix="ff-alert-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    try:
        sound = scratch / "tone.wav"
        write_tone_wav(sound)

        proof.write_ws_config(obs_cfg)
        proof.install_plugin(repo, obs_cfg)
        proof.wait_for_port_free(proof.PORT)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi", "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            asyncio.run(drive(sound, obs_cfg))
            rec_dir = scratch / "rec"
            rec_dir.mkdir()
            quiet_f, loud_f = asyncio.run(record_alert_audio(rec_dir))
            quiet, loud = audio_rms(quiet_f), audio_rms(loud_f)
            check("the sound reaches the actual recording",
                  loud > 0.02 and loud > max(quiet, 1e-6) * 10,
                  f"RMS {loud:.5f} with an alert firing, {quiet:.5f} idle -- a private child "
                  f"source sits in no scene, so this is audio_render handing its mix up")
        finally:
            proof.terminate_process_group(p)
            logs = sorted(obs_cfg.glob("logs/*.txt"))
            if logs:
                text = logs[-1].read_text(errors="replace")
                check("the text source kind is resolved and named in the log",
                      "alerts: drawing text with '" in text,
                      "the log says which kind it picked")
                check("a hostile name is sanitised before it is drawn",
                      "alerts: removed 2 unsafe character(s)" in text,
                      "the test button fires a name carrying U+202E and a newline; both removed")
                fired = text.count("alerts: firing 'Test")
                check("the sanitised name, not the raw one, is what got fired",
                      fired > 0 and "TestViewer42 followed!" in text,
                      f"{fired} firing line(s), and the drawn text reads 'TestViewer42 followed!'")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    print(f"\nalert proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("alert proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
