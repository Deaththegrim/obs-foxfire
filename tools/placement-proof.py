#!/usr/bin/env python3
"""Proves the Placement and Response controls move the picture, in every shader that has them.

Foxfire pack shaders carry an identical block of controls -- Position X/Y, Size, Rotation,
Opacity, Gain, Smoothing, Beat punch -- so that moving between presets does not mean relearning
the panel. OBS's effect language has no #include (packforge's lint refuses it: includes do not
resolve into a pack directory), so that block is COPIED into each file, and copies drift.

Two failures this is here to catch, and neither shows up in a "does it render" check:

  1. A control that exists in the properties panel and changes nothing. That is the single most
     common defect this project keeps finding, and a panel full of dead knobs is worse than a
     panel with none -- a streamer spends an evening deciding their taste is wrong.
  2. The same control meaning different things in different shaders, because one copy of the
     block was edited and the others were not.

So every check runs against EVERY shader in the pack, and reports which ones it inspected.

Measured, over the six shipped packs (one preset per distinct shader file):

    basics   19/19    4 shaders   rotation not measurable on 1 (radial, a ring)
    ember    --/--    2 shaders   baseline STALE: foxfire-ring was removed 2026-09-23 along with
                                  the shader and sigil it owned, so the 14/14 over 3 shaders below
                                  no longer describes this pack. Re-measure before trusting an
                                  ember number -- a fresh run printing a smaller clean sweep must
                                  not read as a pass against a baseline that counted a preset that
                                  no longer exists.
    tune     10/10    2 shaders   rotation measurable on both
    ring      4/4     1 shader    rotation not measurable on 1 (the ring itself)
    overlay   5/5     1 shader    rotation measurable

ARMED: rotation stripped out of ff_place() in a copy of `basics` -- 16/19, the three measurable
shaders all failing, the ring still reported as not measurable. Before that check existed the
same mutant scored a clean 16/16, which is exactly the dead-control defect this file is for.

Rotation is reported as NOT MEASURABLE, never as a pass, when the shape's bounding box is close
to square: a ring turned 90 degrees occupies the same box, so the check could not fail there and
a result that cannot fail is not evidence. The count is printed either way.

Usage:
    tools/placement-proof.py --plugin-build build_x86_64 --pack ../foxfire/packs/basics
"""

import argparse
import asyncio
import base64
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ff_proof  # noqa: E402
import proof  # noqa: E402

from PIL import Image  # noqa: E402

W, H = 640, 360
FAILS: list[str] = []
CHECKS: list[str] = []


def check(name, ok, detail):
    CHECKS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILS.append(name)


# alpha > 8, the same "lit" definition the render proof uses, NOT a tidier-looking 40. A wash is
# a deliberately transparent effect: measured, a shrunk one sits around alpha 40 per pixel, so a
# threshold of 40 made its bounding box appear and disappear with the audio and reported as
# "Position X does nothing". The threshold has to be below the quietest thing being measured, or
# it is measuring itself.
def alpha_mask(img: Image.Image):
    return img.convert("RGBA").getchannel("A").point([255 if i > 8 else 0 for i in range(256)])


def bbox(img):
    return alpha_mask(img).getbbox()


def ink(img) -> int:
    return sum(1 for p in alpha_mask(img).getdata() if p)


def mean_alpha(img) -> float:
    a = list(img.convert("RGBA").getchannel("A").getdata())
    return sum(a) / len(a) if a else 0.0


async def shoot(c, name) -> Image.Image:
    r = await c.request("GetSourceScreenshot", {"sourceName": name, "imageFormat": "png",
                                                "imageWidth": W, "imageHeight": H})
    return Image.open(io.BytesIO(base64.b64decode(r["imageData"].split(",", 1)[1]))).convert("RGBA")


def write_tone_wav(path: Path, seconds=60.0, rate=48000):
    """A chord, not a single tone.

    These shaders read a 64-band log spectrum, and one sine lights about four bands a fifth of
    the way across -- enough to prove a bar exists, not enough to have a shape whose bounding box
    means anything when it moves. Four octaves lit across the range gives the bars a real extent
    to measure. Long enough for one shader's measurements several times over, AND restarted before each
    shader. Both, because neither alone held: a 30s file ran out during the last shader of four,
    and a 4s file with `looping` set ran out DURING a shader -- so whatever looping does here, it
    is not "audio never stops". A shader reading silence reports as "this control does nothing",
    and which shader it hits depends on how many presets come before it, which is the worst kind
    of flake: it moves when you add a preset.
    """
    import math
    import struct
    import wave
    hz = (110.0, 440.0, 1760.0, 7040.0)
    frames = int(seconds * rate)
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(
            struct.pack("<h", int(7000 * sum(math.sin(2 * math.pi * f * i / rate) for f in hz)))
            for i in range(frames)))


async def wait_ready(c, timeout=60.0):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            await c.request("GetSceneList")
            return
        except Exception as e:  # noqa: BLE001
            last = e
            await asyncio.sleep(1.0)
    raise RuntimeError(f"OBS never became ready: {last!r}")


async def settle(c, name, **settings):
    """Applies settings and waits for them to land. The layer parameters are keyed l0.<name>."""
    await c.request("SetInputSettings", {
        "inputName": name, "inputSettings": {f"l0.{k}": v for k, v in settings.items()}})
    await asyncio.sleep(1.2)
    return await shoot(c, name)


async def drive(pack_id: str, presets: list[dict], tone: Path):
    ws, c = await ff_proof.open_client("placement proof")
    inspected = []
    exempt = []
    try:
        await wait_ready(c)
        await c.request("CreateScene", {"sceneName": "pl"})
        await c.request("SetCurrentProgramScene", {"sceneName": "pl"})
        # This proof creates its OWN audio. The first version referenced a source named "tone"
        # that only the other harness makes, so every shader was reading silence -- and bars and
        # the beat wash correctly drew NOTHING, which read as "the controls are broken".
        await c.request("CreateInput", {
            "sceneName": "pl", "inputName": "tone", "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": str(tone), "looping": True}})
        await asyncio.sleep(2.0)

        for pr in presets:
            # Restart the tone for EVERY shader rather than trusting `looping`. Measured: with a
            # 30s file the last shader read silence, and with a 4s looping one it still did --
            # so whatever looping does here, it is not "audio never stops". A shader reading
            # silence reports as "this control does nothing", and which shader it hits depends
            # on how many presets come before it, which is the worst kind of flake.
            await c.request("TriggerMediaInputAction", {
                "inputName": "tone",
                "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
            await asyncio.sleep(1.0)
            name = f"pl-{pr['id']}"
            await c.request("CreateInput", {
                "sceneName": "pl", "inputName": name, "inputKind": "foxfire_visualizer",
                "inputSettings": {"pack": pack_id, "preset": pr["id"], "width": W, "height": H,
                                  "audio_mode": 1, "audio_source": "tone"}})
            # PIN THE SHAPE. A preset that reads the mouth builtins changes what it DRAWS with
            # the audio, and every check below compares two screenshots taken at different
            # moments -- so a moving subject is measured as a moving control. Reproduced: the
            # strip preset reported "Size shrinks the shape: width 102px at size 1.0, 108px at
            # 0.5", which is a real failure of a shader that is correct. Off the tone it draws
            # 216px and 108px, exactly half. The mouth had simply changed shape between the two
            # shots, and a closed mouth is wider than a puckered one.
            #
            # Raising the noise gate past anything the tone can reach rests the mouth on one
            # shape for the whole measurement. It is set on every preset, because it costs
            # nothing on a shader with no mouth in it -- ff_renderer_apply_settings reads these
            # whether or not any layer asks for them.
            await c.request("SetInputSettings", {
                "inputName": name, "inputSettings": {"mouth.gate": 1.0}})
            await asyncio.sleep(1.5)
            inspected.append(pr["id"])

            base = await settle(c, name, pos_x=0.0, pos_y=0.0, size=1.0, rotate_deg=0.0,
                                opacity=1.0, gain=1.0)
            b0 = bbox(base)
            if not b0:
                check(f"{pr['id']}: draws something to move", False, "nothing on screen to measure")
                await c.request("RemoveInput", {"inputName": name})
                continue

            # Position. A shape filling the canvas cannot be seen to move, so it is shrunk first
            # -- which also means Size has to work for this check to be meaningful at all.
            small = await settle(c, name, size=0.5)
            bs = bbox(small)
            check(f"{pr['id']}: Size shrinks the shape",
                  bs is not None and (bs[2] - bs[0]) < (b0[2] - b0[0]) * 0.9,
                  f"width {b0[2]-b0[0]}px at size 1.0, {(bs[2]-bs[0]) if bs else None}px at 0.5")

            right = await settle(c, name, pos_x=0.3)
            br = bbox(right)
            cx0 = (bs[0] + bs[2]) / 2 if bs else 0
            cxr = (br[0] + br[2]) / 2 if br else 0
            check(f"{pr['id']}: Position X moves it right",
                  br is not None and cxr > cx0 + 20,
                  f"centre x {cx0:.0f}px -> {cxr:.0f}px for pos_x 0 -> 0.3")

            up = await settle(c, name, pos_x=0.0, pos_y=0.3)
            bu = bbox(up)
            cy0 = (bs[1] + bs[3]) / 2 if bs else 0
            cyu = (bu[1] + bu[3]) / 2 if bu else 0
            check(f"{pr['id']}: Position Y moves it UP, not down",
                  bu is not None and cyu < cy0 - 20,
                  f"centre y {cy0:.0f}px -> {cyu:.0f}px for pos_y 0 -> 0.3 (smaller y is higher "
                  f"on screen; +Y up is what every other OBS position control does)")

            # Rotation. A bounding box can only show a rotation if the shape is not square to
            # begin with: a ring is rotationally symmetric and a 90-degree turn leaves its box
            # exactly where it was, so for those this reports NOT MEASURABLE rather than passing
            # -- a check that cannot fail on a shape is not evidence about that shape.
            flat = await settle(c, name, pos_y=0.0, rotate_deg=0.0)
            bf = bbox(flat)
            turned = await settle(c, name, rotate_deg=90.0)
            bt = bbox(turned)
            ratio0 = (bf[2] - bf[0]) / max(bf[3] - bf[1], 1) if bf else 1.0
            if 0.85 < ratio0 < 1.18:
                exempt.append(f"{pr['id']} (box is {ratio0:.2f}:1, too square to read a turn)")
                print(f"  [ -- ] {pr['id']}: Rotation NOT MEASURABLE by bounding box: the shape's "
                      f"box is {ratio0:.2f}:1, so a 90 degree turn moves nothing measurable here")
            else:
                ratio90 = (bt[2] - bt[0]) / max(bt[3] - bt[1], 1) if bt else ratio0
                check(f"{pr['id']}: Rotation turns the shape",
                      bt is not None and (ratio90 < ratio0 * 0.7 or ratio90 > ratio0 * 1.43),
                      f"box {ratio0:.2f}:1 at 0 degrees, {ratio90:.2f}:1 at 90")

            await settle(c, name, rotate_deg=0.0)
            dim = await settle(c, name, pos_y=0.0, opacity=0.35)
            check(f"{pr['id']}: Opacity fades it",
                  mean_alpha(dim) < mean_alpha(small) * 0.6,
                  f"mean alpha {mean_alpha(small):.1f} at 1.0, {mean_alpha(dim):.1f} at 0.35")

            await settle(c, name, opacity=1.0, size=1.0)
            await c.request("RemoveInput", {"inputName": name})
    finally:
        await ws.close()
    return inspected, exempt


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    ap.add_argument("--pack", required=True, help="a pack dir whose shaders carry the block")
    ap.add_argument("--presets", default="",
                    help="comma-separated preset ids; default is one per distinct effect file")
    args = ap.parse_args()

    repo = Path(args.plugin_build).resolve()
    pack_dir = Path(args.pack).resolve()
    manifest = json.loads((pack_dir / "pack.json").read_text())
    pack_id = manifest["id"]

    if args.presets:
        want = set(args.presets.split(","))
        presets = [p for p in manifest["presets"] if p["id"] in want]
    else:
        # one preset per distinct effect FILE: the block is copied per file, so per-file is the
        # unit that can drift. Testing all 15 presets of a 4-shader pack would take four times
        # as long to learn the same four things.
        seen, presets = set(), []
        for p in manifest["presets"]:
            eff = p["layers"][0]["effect"]
            if eff not in seen:
                seen.add(eff)
                presets.append(p)

    cfg = Path(tempfile.mkdtemp(prefix="ff-place-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    scratch = Path(tempfile.mkdtemp(prefix="ff-place-src-"))
    tone = scratch / "tone.wav"
    write_tone_wav(tone)
    try:
        proof.write_ws_config(obs_cfg)
        proof.install_plugin(repo, obs_cfg)
        proof.install_pack(pack_dir, obs_cfg)
        proof.wait_for_port_free(proof.PORT)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi", "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            inspected, exempt = asyncio.run(drive(pack_id, presets, tone))
        finally:
            proof.terminate_process_group(p)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    print(f"\ninspected {len(inspected)} shader(s) of '{pack_id}': {', '.join(inspected)}")
    # A gate says how much it actually inspected. Rotation is the one control here that some
    # shapes cannot be measured for, so the count of those is printed rather than left implied.
    print(f"rotation not measurable on {len(exempt)} of {len(inspected)} shader(s)"
          + (": " + "; ".join(exempt) if exempt else ""))
    print(f"placement proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("placement proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
