#!/usr/bin/env python3
"""Animated art actually MOVES -- animated GIF and animated WebP, in a real OBS.

`gs_image_file_init` decodes an animation, but libobs leaves advancing it to the host: without
`gs_image_file_tick` + `gs_image_file_update_texture` the texture holds frame 0 for ever. Foxfire
never called either, so every animated asset a pack or a viewer supplied rendered as one frozen
frame -- and the file picker made it worse by excluding `*.gif` (honest) while accepting `*.webp`,
which was taken and then silently held still. `tick_animations` in ff-layers.c drives it now.

"The file loads" is not the claim. The claim is that the picture CHANGES, so this measures frames:
several screenshots across one loop, counting how many are distinct.

The still PNG is the point of the whole file. Without it "the frames differ" would also pass for a
renderer emitting noise, a flickering shader, or a texture bound to garbage -- so a preset whose
art cannot move is checked to produce exactly ONE distinct frame. A run where the animated presets
move and the still one does not is the only shape that means what it says.

    animation-proof.py --plugin-build .
"""
import argparse
import asyncio
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import proof  # noqa: E402
import ff_proof  # noqa: E402

# Three frames of flat, maximally different colour. Flat because a partial decode or a half-updated
# texture shows up as a colour nothing authored, and maximally different so "did it change" needs no
# threshold tuning -- the frames are further apart than any compression artefact.
FRAME_RGB = [(255, 0, 0), (0, 255, 0), (0, 0, 255)]
FRAME_MS = 100
SHOTS = 9
SHOT_GAP = 0.12  # 9 x 120ms = ~1.08s, three full loops of a 300ms animation

# (preset id, art file, expected distinct frames)
#   ">1" -- must animate
#   "1"  -- must render and hold still
#   "0"  -- must FAIL to load; recorded as a capability, not skipped
#
# Animated WebP was a "0" until the engine grew its own decoder, and the flip is exactly what that
# expectation was written for. libobs still cannot do it -- it decodes GIF with libnsgif, routes
# every other image through ffmpeg, and ffmpeg has no animated-webp decoder, so `ffprobe` refuses
# such a file on its own. ff-webp.c carries libwebp's WebPAnimDecoder instead. GIF is not an
# acceptable substitute for art: 256 colours and one bit of alpha ruin gradients and soft edges.
PRESETS = [
    ("anim-gif", "art.gif", ">1"),
    ("anim-webp", "art.webp", ">1"),
    ("still-webp", "still.webp", "1"),
    ("still-png", "art.png", "1"),  # the dead control -- must render and NOT move
]

EFFECT = """uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d spectrum;
uniform texture2d waveform;
uniform float2 uv_size;
uniform float time;
uniform float level;
uniform float bass;
uniform float beat;
sampler_state linS { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertData VSDefault(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
uniform texture2d art <string path="textures/%s"; bool user = true; string label="Art";>;
uniform float2 art_size;
float4 PSDraw(VertData v) : TARGET
{
	/* The art, full frame, and NOTHING else -- no time, no audio, no noise. Anything that moved
	   on its own would make "the frames differ" true without the animation decoder doing a
	   thing, which is the failure the still-PNG control exists to catch. */
	return art.Sample(linS, v.uv);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSDraw(v); } }
"""


def write_art(tex_dir: Path) -> None:
    from PIL import Image
    tex_dir.mkdir(parents=True, exist_ok=True)
    frames = [Image.new("RGBA", (64, 64), (*c, 255)) for c in FRAME_RGB]
    frames[0].save(tex_dir / "art.gif", save_all=True, append_images=frames[1:],
                   duration=FRAME_MS, loop=0, disposal=2)
    frames[0].save(tex_dir / "art.webp", save_all=True, append_images=frames[1:],
                   duration=FRAME_MS, loop=0, lossless=True)
    # the control: one frame, same size, same colour as the animation's first
    frames[0].save(tex_dir / "art.png")
    frames[0].save(tex_dir / "still.webp", lossless=True)


def write_pack(root: Path) -> Path:
    pack = root / "ffanim"
    (pack / "effects").mkdir(parents=True, exist_ok=True)
    presets = []
    for pid, art, _ in PRESETS:
        (pack / "effects" / f"{pid}.effect").write_text(EFFECT % art)
        presets.append({"id": pid, "name": pid, "kind": "visualizer",
                        "layers": [{"effect": f"effects/{pid}.effect", "params": {}}]})
    write_art(pack / "textures")
    (pack / "pack.json").write_text(json.dumps({
        # Every one of these is load-bearing and the loader says so by name. The first two runs
        # of this proof failed on the MANIFEST, not the animation: "format must be 1", then
        # "min_engine must be x.y.z" (it is a version string, not a number). Both surfaced to the
        # user as "Pack 'ffanim' is not installed", so a fixture that is subtly wrong looks
        # exactly like a feature that does not work.
        "format": 1, "id": "ffanim", "name": "Foxfire Animation Fixture", "version": "1.0.0",
        "author": "foxfire", "min_engine": "0.1.0", "licensed": False, "released": 0,
        "kinds": ["visualizer"], "presets": presets}, indent=2))
    return pack


async def distinct_frames(client, source: str) -> tuple[int, list[str]]:
    """How many DISTINCT pictures the source produced across one loop."""
    seen = []
    for _ in range(SHOTS):
        raw = await proof._shoot(client, source)
        seen.append(hashlib.sha256(raw).hexdigest()[:12])
        await asyncio.sleep(SHOT_GAP)
    return len(set(seen)), seen


async def drive(port: int) -> dict:
    out: dict = {}
    ws, client = await ff_proof.open_client("animation proof")
    try:
        scene = (await client.request("GetSceneList"))["currentProgramSceneName"]
        for pid, _, _expect in PRESETS:
            name = f"ffanim_{pid}"
            await client.request("CreateInput", {
                "sceneName": scene, "inputName": name, "inputKind": "foxfire_visualizer",
                "inputSettings": {"pack": "ffanim", "preset": pid, "width": 320, "height": 180,
                                  "audio_mode": 0}})
            await asyncio.sleep(1.0)  # let it load and render a frame
            n, seen = await distinct_frames(client, name)
            out[pid] = {"distinct": n, "hashes": seen}
    finally:
        await ws.close()
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    ap.add_argument("--keep-config", action="store_true",
                    help="leave the sandbox config in place, to read OBS's log")
    a = ap.parse_args()

    cfg = Path(tempfile.mkdtemp(prefix="ff-anim-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    try:
        proof.install_plugin(Path(a.plugin_build).resolve(), obs_cfg)
        pack = write_pack(cfg)
        proof.install_pack(pack, obs_cfg)
        proof.write_ws_config(obs_cfg)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi",
             "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(ff_proof.PORT, 90)
            res = asyncio.run(drive(ff_proof.PORT))
        finally:
            proof.terminate_process_group(p)
        # Whether each file DECODED, read from OBS's own log. A texture that failed to load renders
        # as a blank source, which counts as exactly one distinct frame -- indistinguishable from
        # a still image by pixels alone. That is how the first run of this proof read "1 distinct
        # frame" for the animated WebP and looked like a frozen animation rather than a file OBS
        # could not open at all.
        logs = sorted(obs_cfg.glob("logs/*.txt"))
        log = logs[-1].read_text(errors="replace") if logs else ""
        loaded_files = {pid for pid, art, _ in PRESETS if f"textures/{art}" not in
                        "".join(l for l in log.splitlines() if "failed to load" in l.lower())}
    finally:
        if a.keep_config:
            print(f"sandbox kept at {cfg}")
        else:
            shutil.rmtree(cfg, ignore_errors=True)

    fails = 0
    for pid, art, expect in PRESETS:
        r = res.get(pid)
        if not r:
            print(f"  [FAIL] {pid}: the source produced no frames at all")
            fails += 1
            continue
        n = r["distinct"]
        loaded = pid in loaded_files
        if expect == ">1":
            ok, why = n >= 2, (f"{n} distinct frame(s) over {SHOTS} shots -- animated art must "
                               f"CHANGE; 1 means the decoder was never ticked")
        elif expect == "1":
            ok, why = (loaded and n == 1), (
                f"{n} distinct frame(s), loaded={loaded} -- must render and hold still; more than "
                f"1 means something other than the animation is moving and every check above "
                f"proves nothing")
        else:  # "0" -- a format we do NOT support
            ok, why = (not loaded), (
                f"loaded={loaded} -- ffmpeg cannot decode animated WebP, so this must fail to "
                f"load and say so. If this now PASSES the format gained support and this "
                f"expectation is the thing to update")
        if not ok:
            fails += 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {pid} ({art}): {why}")

    print(f"\nanimation proof: {len(PRESETS) - fails}/{len(PRESETS)} checks")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
