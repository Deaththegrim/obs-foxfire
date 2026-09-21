#!/usr/bin/env python3
"""Proves a viewer-supplied image really replaces a pack's art, at the right shape.

Self-contained: it generates its own pack, its own pack art and its own stand-in for the file a
viewer would pick, so nothing here depends on art that ships in the repository. The two images
are given deliberately different aspects (1:1 against 3:1) because the aspect is the assertion
that actually bites.

Four states, all measured:
  1. nothing picked              -> the pack's own art
  2. a file picked               -> that image, AT ITS OWN ASPECT
  3. the setting cleared         -> the pack's art back
  4. a path that cannot be read  -> the pack's art, and a warning naming the file

Why the aspect check matters more than "did the picture change": a shader cannot ask a texture
its own size on the OpenGL backend (GetDimensions does not compile; see
foxfire/research/shader-compat.md), so the engine feeds a `<name>_size` uniform instead. If that
feed breaks, the viewer's image is still drawn -- just squashed to the pack art's proportions --
and a check that only asked "is it different?" would pass while the feature was broken.

Usage:
    tools/user-image-proof.py --plugin-build build_x86_64
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

from PIL import Image, ImageDraw  # noqa: E402

W, H = 640, 360
FAILS = []
CHECKS = []

EFFECT = """uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 uv_size;
uniform texture2d art <string path = "art/pack.png"; bool user = true; string label="Art";>;
uniform float2 art_size;
uniform float scale <string label="Size"; float minimum=0.05; float maximum=1.0; float step=0.01;> = 0.5;
sampler_state linSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertData VSDefault(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
float4 PSDraw(VertData v) : TARGET
{
\tfloat aspect = uv_size.x / max(uv_size.y, 1.0);
\tfloat2 p = v.uv - 0.5;
\tp.x = p.x * aspect;
\tfloat2 uv = p / max(scale, 0.0001);
\tuv.x = uv.x / max(art_size.x / max(art_size.y, 1.0), 0.0001);
\tuv = uv + 0.5;
\tfloat inside = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);
\tfloat4 c = art.Sample(linSampler, uv);
\tfloat a = c.a * inside;
\treturn float4(1.0, 1.0, 1.0, a);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSDraw(v); } }
"""


def check(name, ok, detail):
    CHECKS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILS.append(name)


def make_art(path: Path, w: int, h: int) -> None:
    """A filled rectangle inset from the edge: its alpha bounding box is a known aspect."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(im).rectangle([w // 20, h // 20, w - w // 20, h - h // 20], fill=(255, 255, 255, 255))
    im.save(path)


def build_pack(root: Path) -> Path:
    pack = root / "userimg"
    (pack / "effects").mkdir(parents=True)
    (pack / "art").mkdir()
    (pack / "effects" / "art.effect").write_text(EFFECT)
    make_art(pack / "art" / "pack.png", 512, 512)  # 1:1
    (pack / "pack.json").write_text(json.dumps({
        "format": 1, "id": "userimg", "name": "User image proof", "version": "0.1.0",
        "author": "proof", "min_engine": "0.1.0", "licensed": False, "kinds": ["visualizer"],
        "presets": [{"id": "art", "name": "art", "kind": "visualizer", "thumb": "", "heavy": False,
                     "layers": [{"effect": "effects/art.effect", "params": {}}]}],
    }))
    return pack


async def shoot(c, name) -> Image.Image:
    r = await c.request("GetSourceScreenshot", {"sourceName": name, "imageFormat": "png",
                                                "imageWidth": W, "imageHeight": H})
    return Image.open(io.BytesIO(base64.b64decode(r["imageData"].split(",", 1)[1]))).convert("RGBA")


def art_aspect(img: Image.Image):
    lut = [255 if i > 40 else 0 for i in range(256)]
    bb = img.getchannel("A").point(lut).getbbox()
    if not bb:
        return None
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    return w / h if h else None


async def wait_ready(c, timeout=60.0):
    """The websocket port opens before OBS finishes starting, and requests in that window come
    back 207 'OBS is not ready'. Poll something harmless rather than sleeping a guessed amount."""
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


async def drive(userart: Path):
    ws, c = await ff_proof.open_client("user image proof")
    try:
        await wait_ready(c)
        await c.request("CreateScene", {"sceneName": "ui"})
        await c.request("SetCurrentProgramScene", {"sceneName": "ui"})
        await c.request("CreateInput", {
            "sceneName": "ui", "inputName": "ff", "inputKind": "foxfire_visualizer",
            "inputSettings": {"pack": "userimg", "preset": "art", "width": W, "height": H,
                              "audio_mode": 0}})
        await asyncio.sleep(2.0)

        pack_ar = art_aspect(await shoot(c, "ff"))
        check("the pack's own art renders", pack_ar is not None and abs(pack_ar - 1.0) < 0.2,
              f"aspect={pack_ar}, the pack file is 512x512 = 1.00")

        # the real trigger path: the property is a file picker, so a path string is what OBS writes
        await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"l0.art": str(userart)}})
        await asyncio.sleep(2.0)
        user_ar = art_aspect(await shoot(c, "ff"))
        check("the viewer's image is drawn at ITS OWN aspect, not the pack art's",
              user_ar is not None and abs(user_ar - 3.0) < 0.4,
              f"aspect={user_ar}, the picked file is 900x300 = 3.00, the pack's is 1.00")

        await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"l0.art": ""}})
        await asyncio.sleep(2.0)
        back_ar = art_aspect(await shoot(c, "ff"))
        check("clearing the picker restores the pack's art",
              back_ar is not None and abs(back_ar - 1.0) < 0.2, f"aspect={back_ar}")

        await c.request("SetInputSettings",
                        {"inputName": "ff", "inputSettings": {"l0.art": "/nonexistent/nope.png"}})
        await asyncio.sleep(2.0)
        miss_ar = art_aspect(await shoot(c, "ff"))
        check("an unreadable file falls back to the pack's art instead of blanking",
              miss_ar is not None and abs(miss_ar - 1.0) < 0.2, f"aspect={miss_ar}")
    finally:
        await ws.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True, help="built engine repo (has install-local.sh)")
    args = ap.parse_args()

    repo = Path(args.plugin_build).resolve()
    scratch = Path(tempfile.mkdtemp(prefix="ff-userimg-src-"))
    cfg = Path(tempfile.mkdtemp(prefix="ff-userimg-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    try:
        pack = build_pack(scratch)
        userart = scratch / "picked.png"
        make_art(userart, 900, 300)  # 3:1, unmistakably not the pack's

        proof.write_ws_config(obs_cfg)
        proof.install_plugin(repo, obs_cfg)
        proof.install_pack(pack, obs_cfg)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi", "--minimize-to-tray"],
            # --multi: without it a second OBS opens an "already running" warning dialog; under
            # Xvfb nobody can click it, so it hangs to the timeout and never writes a log.
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            asyncio.run(drive(userart))
        finally:
            proof.terminate_process_group(p)
            logs = sorted(obs_cfg.glob("logs/*.txt"))
            if logs:
                want = "could not be loaded; using the pack's own art"
                fell_back = any(want in ln for ln in logs[-1].read_text(errors="replace").splitlines())
                check("the unreadable file is reported, not swallowed", fell_back,
                      f"log contains {want!r}: {fell_back}")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    print(f"\nuser-image proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("user-image proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
