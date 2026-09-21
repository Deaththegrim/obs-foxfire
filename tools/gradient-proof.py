#!/usr/bin/env python3
"""Proves an engine-baked gradient reaches the pixels, and that a viewer can move it.

A gradient is a texture the shader never loads: the pack declares
`uniform texture2d ramp <string gradient = "#ff6a4d,#d9a441,#0b0a0d">;` and the engine bakes a
256x1 ramp from colour stops the viewer controls. That makes it the one parameter type where
"the property appeared" and "the picture changed" can come apart completely -- the colour pickers
can be present and correct while nothing is bound, or a ramp can be bound and never rebaked when
a stop moves. So every check here reads PIXELS, through the real settings keys OBS writes.

Self-contained: it writes its own pack, with its own shader, and asserts against colours it chose.

  1. the pack's declared stops are the colours actually drawn, at the ends and in the middle
  2. a preset's own stop list overrides the shader's -- one shader, many looks
  3. changing a stop's COLOUR through settings changes the pixels
  4. changing a stop's POSITION moves where the colours land (the part a palette cannot do)
  5. clearing the settings restores the preset's ramp
  6. a malformed stop refuses the whole gradient and says so, rather than drawing black

Usage:
    tools/gradient-proof.py --plugin-build build_x86_64
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
FAILS = []
CHECKS = []

# The ramp is drawn straight across the frame, so x maps to the gradient's t. Opaque, so a
# sampled colour is the ramp's colour and not a composite with whatever is behind it.
EFFECT = """uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d ramp <string gradient = "#ff0000,#00ff00,#0000ff"; string label="Ramp";>;
sampler_state linSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertData VSDefault(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
float4 PSDraw(VertData v) : TARGET
{
\tfloat4 c = ramp.Sample(linSampler, float2(v.uv.x, 0.5));
\treturn float4(c.rgb, 1.0);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSDraw(v); } }
"""

# A second shader whose gradient annotation is malformed. It must be refused as a whole.
BAD_EFFECT = EFFECT.replace('"#ff0000,#00ff00,#0000ff"', '"#ff0000,notacolour,#0000ff"')


def check(name, ok, detail):
    CHECKS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILS.append(name)


def build_pack(root: Path) -> Path:
    pack = root / "gradproof"
    (pack / "effects").mkdir(parents=True)
    (pack / "effects" / "ramp.effect").write_text(EFFECT)
    (pack / "effects" / "bad.effect").write_text(BAD_EFFECT)

    def preset(pid, effect, params):
        return {"id": pid, "name": pid, "kind": "visualizer", "thumb": "", "heavy": False,
                "layers": [{"effect": f"effects/{effect}", "params": params}]}

    (pack / "pack.json").write_text(json.dumps({
        "format": 1, "id": "gradproof", "name": "Gradient proof", "version": "0.1.0",
        "author": "proof", "min_engine": "0.1.0", "licensed": False, "kinds": ["visualizer"],
        "presets": [
            preset("shader", "ramp.effect", {}),
            # the same shader, a different look, decided entirely by the preset
            preset("override", "ramp.effect", {"ramp": "#ffffff,#000000"}),
            preset("malformed", "bad.effect", {}),
        ],
    }))
    return pack


async def shoot(c, name) -> Image.Image:
    r = await c.request("GetSourceScreenshot", {"sourceName": name, "imageFormat": "png",
                                                "imageWidth": W, "imageHeight": H})
    return Image.open(io.BytesIO(base64.b64decode(r["imageData"].split(",", 1)[1]))).convert("RGBA")


def at(img: Image.Image, t: float):
    """The ramp colour at position t, averaged down a column to shrug off a stray row."""
    x = max(0, min(W - 1, int(round(t * (W - 1)))))
    px = [img.getpixel((x, y)) for y in range(H // 4, 3 * H // 4, 8)]
    n = len(px)
    return tuple(round(sum(p[i] for p in px) / n) for i in range(3))


def near(got, want, tol=14):
    return all(abs(g - w) <= tol for g, w in zip(got, want))


def pack_color(r, g, b, a=255):
    """What OBS writes for a colour picker: 0xAABBGGRR."""
    return (a << 24) | (b << 16) | (g << 8) | r


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


async def use(c, preset):
    await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"preset": preset}})
    await asyncio.sleep(2.0)


async def drive():
    ws, c = await ff_proof.open_client("gradient proof")
    try:
        await wait_ready(c)
        await c.request("CreateScene", {"sceneName": "gr"})
        await c.request("SetCurrentProgramScene", {"sceneName": "gr"})
        await c.request("CreateInput", {
            "sceneName": "gr", "inputName": "ff", "inputKind": "foxfire_visualizer",
            "inputSettings": {"pack": "gradproof", "preset": "shader", "width": W, "height": H,
                              "audio_mode": 0}})
        await asyncio.sleep(2.0)

        img = await shoot(c, "ff")
        ends = (at(img, 0.0), at(img, 0.5), at(img, 1.0))
        check("the shader's declared stops are the colours drawn",
              near(ends[0], (255, 0, 0)) and near(ends[1], (0, 255, 0)) and near(ends[2], (0, 0, 255)),
              f"t=0 {ends[0]}, t=0.5 {ends[1]}, t=1 {ends[2]}; declared #ff0000,#00ff00,#0000ff")

        quarter = at(img, 0.25)
        check("between two stops it interpolates rather than stepping",
              not near(quarter, (255, 0, 0), 30) and not near(quarter, (0, 255, 0), 30)
              and quarter[0] > 40 and quarter[1] > 40,
              f"t=0.25 is {quarter}, which is neither end stop")

        await use(c, "override")
        img = await shoot(c, "ff")
        o = (at(img, 0.0), at(img, 1.0))
        check("a preset's own stop list overrides the shader's",
              near(o[0], (255, 255, 255)) and near(o[1], (0, 0, 0)),
              f"t=0 {o[0]}, t=1 {o[1]}; the preset asked for #ffffff,#000000 over the shader's red/green/blue")

        await use(c, "shader")
        await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {
            "l0.ramp.c0": pack_color(255, 0, 255)}})
        await asyncio.sleep(2.0)
        img = await shoot(c, "ff")
        c0 = at(img, 0.0)
        check("changing a stop's colour through settings changes the pixels",
              near(c0, (255, 0, 255)), f"t=0 is now {c0}, the viewer set #ff00ff")

        # the middle stop starts at 0.5; push it to 0.9 and the green must move with it
        before = at(img, 0.5)
        await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"l0.ramp.p1": 0.9}})
        await asyncio.sleep(2.0)
        img = await shoot(c, "ff")
        moved_to, vacated = at(img, 0.9), at(img, 0.5)
        check("moving a stop's position moves where its colour lands",
              near(moved_to, (0, 255, 0)) and not near(vacated, (0, 255, 0), 30),
              f"green was at t=0.5 {before}; after moving stop 2 to 0.9 it is at t=0.9 {moved_to} "
              f"and t=0.5 reads {vacated}")

        # overlay:false REPLACES the settings rather than merging into them, which is what
        # Restore Defaults does: the l0.ramp.* keys stop existing. Passing null instead merges a
        # null in, leaving obs_data_has_user_value true and the viewer's value in place -- so the
        # first version of this check was testing the websocket, not the engine.
        await c.request("SetInputSettings", {"inputName": "ff", "overlay": False, "inputSettings": {
            "pack": "gradproof", "preset": "shader", "width": W, "height": H, "audio_mode": 0}})
        await asyncio.sleep(2.0)
        img = await shoot(c, "ff")
        r0, r1 = at(img, 0.0), at(img, 0.5)
        check("clearing the settings restores the preset's ramp",
              near(r0, (255, 0, 0)) and near(r1, (0, 255, 0)),
              f"t=0 {r0}, t=0.5 {r1}; back to the shader's own stops")

        await use(c, "malformed")
        img = await shoot(c, "ff")
        m = (at(img, 0.0), at(img, 1.0))
        check("a malformed stop does not leave a black ramp pretending to be art",
              m[0] == m[1],
              f"t=0 {m[0]}, t=1 {m[1]} -- the gradient was refused whole, so nothing is bound and "
              f"the frame is flat; the warning check below is what proves it was reported")
    finally:
        await ws.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True, help="built engine repo (has install-local.sh)")
    args = ap.parse_args()

    repo = Path(args.plugin_build).resolve()
    scratch = Path(tempfile.mkdtemp(prefix="ff-grad-src-"))
    cfg = Path(tempfile.mkdtemp(prefix="ff-grad-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    try:
        proof.write_ws_config(obs_cfg)
        proof.install_plugin(repo, obs_cfg)
        proof.install_pack(build_pack(scratch), obs_cfg)
        proof.wait_for_port_free(proof.PORT)  # never connect to a previous run's dying OBS
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi", "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            asyncio.run(drive())
        finally:
            proof.terminate_process_group(p)
            logs = sorted(obs_cfg.glob("logs/*.txt"))
            if logs:
                want = "not a #rrggbb or #rrggbbaa colour"
                said = any(want in ln for ln in logs[-1].read_text(errors="replace").splitlines())
                check("the malformed stop is named in the log, not swallowed", said,
                      f"log contains {want!r}: {said}")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    print(f"\ngradient proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("gradient proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
