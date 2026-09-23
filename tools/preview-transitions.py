#!/usr/bin/env python3
"""Every transition preset in a pack, driven through a real OBS, recorded so a human can watch it.

tools/transition-proof.py answers "does the transition work" for three presets and reports
numbers. This answers the other question -- "what does it LOOK like" -- for all of them, because
a preset that measures correctly and looks wrong is a preset that ships wrong, and nothing in the
repo could show a preset in motion before this.

It is a PREVIEW, not a gate: it asserts nothing and returns 0 as long as the recording happened.
What it produces is

    <out>/<pack>-all.mp4       every preset, in order, cut over two contrasting scenes
    <out>/<pack>-sheet.png     a contact sheet, one mid-transition frame per preset, labelled
    <out>/frames/<id>.png      those frames on their own

Scenes A and B are a red and a blue card, deliberately flat and deliberately unlike each other:
anything in the recorded middle that is neither is the transition's own art, and with flat scenes
there is nothing else it could be.

    preview-transitions.py --plugin-build ~/vault/projects/obs-foxfire --pack ../foxfire/packs/ember
"""
import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ff_proof  # noqa: E402
import proof  # noqa: E402


def _load_proof_helpers():
    """transition-proof.py itself, imported by path for its helpers.

    Its filename has a hyphen so it cannot be imported by name, and copying bgra(), frame_at(),
    start_recording() and unlicense_installed_copy() in here would be a second copy of the exact
    arithmetic the proof is trusted for. Importing it is also what keeps A_RGB/B_RGB the same two
    scenes in both files -- a preview shot over different colours than the proof measures is a
    preview of something else.
    """
    import importlib.util
    f = Path(__file__).resolve().parent / "transition-proof.py"
    spec = importlib.util.spec_from_file_location("transition_proof", f)
    assert spec and spec.loader, f"{f} is not importable"
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


tps = _load_proof_helpers()

from PIL import Image, ImageDraw  # noqa: E402

DURATION_MS = 2500
SETTLE_S = 1.2


def scene_collection(name, transitions):
    """transition-proof.py's collection, minus the audio tones and with one transition per preset.

    OBS owns the transition list and obs-websocket cannot add to it, so every preset has to be
    seeded here, before OBS starts, and selected by name once it is up.
    """
    def colour(src, rgb):
        return {"name": src, "id": "color_source_v3", "versioned_id": "color_source_v3",
                "settings": {"color": tps.bgra(rgb), "width": 640, "height": 360},
                "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
                "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
                "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {},
                "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
                "private_settings": {}}

    def item(src, item_id):
        return {"name": src, "source_uuid": "", "visible": True, "locked": False,
                "rot": 0.0, "pos": {"x": 0.0, "y": 0.0}, "scale": {"x": 1.0, "y": 1.0},
                "align": 5, "bounds_type": 0, "bounds_align": 0,
                "bounds": {"x": 0.0, "y": 0.0}, "crop_left": 0, "crop_top": 0,
                "crop_right": 0, "crop_bottom": 0, "id": item_id,
                "group_item_backup": False, "scale_filter": "disable",
                "blend_method": "default", "blend_type": "normal",
                "show_transition": {"duration": 0}, "hide_transition": {"duration": 0},
                "private_settings": {}}

    def scene(scene_name, child):
        return {"name": scene_name, "id": "scene", "versioned_id": "scene",
                "settings": {"id_counter": 2, "custom_size": False, "items": [item(child, 1)]},
                "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
                "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
                "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {},
                "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
                "private_settings": {}}

    return {
        "name": name, "current_scene": "A", "current_program_scene": "A",
        "current_transition": transitions[0]["name"], "transition_duration": DURATION_MS,
        "scene_order": [{"name": "A"}, {"name": "B"}],
        "sources": [colour("solidA", tps.A_RGB), colour("solidB", tps.B_RGB),
                    scene("A", "solidA"), scene("B", "solidB")],
        "transitions": transitions,
        "groups": [], "quick_transitions": [], "saved_projectors": [], "canvases": [],
        "preview_locked": False, "scaling_enabled": False, "scaling_level": 0,
        "scaling_off_x": 0.0, "scaling_off_y": 0.0, "virtual-camera": {"type2": 3},
        "resolution": {"x": 640, "y": 360}, "version": 2, "modules": {},
    }


def write_collection(obs_cfg: Path, transitions):
    d = obs_cfg / "basic" / "scenes"
    d.mkdir(parents=True, exist_ok=True)
    (d / "Untitled.json").write_text(json.dumps(scene_collection("Untitled", transitions), indent=4))
    p = obs_cfg / "basic" / "profiles" / "Untitled"
    p.mkdir(parents=True, exist_ok=True)
    (p / "basic.ini").write_text(
        "[General]\nName=Untitled\n"
        "[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSCommon=30\n"
        "[Output]\nMode=Simple\n[SimpleOutput]\nRecFormat2=mkv\nRecQuality=Small\nRecEncoder=x264\n")
    (obs_cfg / "global.ini").write_text(
        "[Basic]\nProfile=Untitled\nProfileDir=Untitled\nSceneCollection=Untitled\n"
        "SceneCollectionFile=Untitled\n")


async def drive(rec_dir: Path, presets, timeline: list):
    """One recording, every preset in turn, and the wall-clock middle of each cut written down.

    One recording rather than one per preset because booting OBS is by far the slowest part of
    this, and the timeline is recorded as it happens rather than computed afterwards: the awaits
    below do not take exactly as long as they ask for, and a frame pulled from a predicted
    timestamp lands next to the cut instead of inside it.
    """
    import time
    ws, c = await ff_proof.open_client("transition preview")
    try:
        names = {t["transitionName"] for t in (await c.request("GetSceneTransitionList"))["transitions"]}
        missing = [p["id"] for p in presets if p["id"] not in names]
        if missing:
            print(f"  NOTE: OBS did not register {len(missing)} transition(s): {missing}")

        await c.request("SetCurrentProgramScene", {"sceneName": "A"})
        await asyncio.sleep(SETTLE_S)
        await c.request("SetRecordDirectory", {"recordDirectory": str(rec_dir)})
        await tps.start_recording(c)
        t0 = time.monotonic()
        await asyncio.sleep(SETTLE_S)

        secs = DURATION_MS / 1000.0
        target = "B"
        for p in presets:
            if p["id"] not in names:
                continue
            await c.request("SetCurrentSceneTransition", {"transitionName": p["id"]})
            await c.request("SetCurrentSceneTransitionDuration", {"transitionDuration": DURATION_MS})
            start = time.monotonic() - t0
            await c.request("SetCurrentProgramScene", {"sceneName": target})
            await asyncio.sleep(secs + SETTLE_S)
            # Two moments, because one cannot show a covering sweep. At the swap the frame is
            # solid fill by design -- that IS the effect -- so the leading edge, which is the part
            # the band art draws and the part a buyer is choosing between, appears in no frame at
            # all. `early` catches it mid-travel.
            timeline.append({"id": p["id"], "name": p["name"],
                             "early": start + secs * 0.32, "mid": start + secs / 2.0})
            target = "A" if target == "B" else "B"

        await c.request("StopRecord")
        await tps.wait_recording_stopped(c)
    finally:
        await ws.close()


def label(img: Image.Image, text: str) -> Image.Image:
    out = img.copy()
    d = ImageDraw.Draw(out)
    w, h = out.size
    d.rectangle([0, h - 18, w, h], fill=(0, 0, 0))
    d.text((5, h - 15), text, fill=(255, 255, 255))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    ap.add_argument("--pack", required=True, type=Path)
    ap.add_argument("--out", type=Path, default=Path("preview-out"))
    args = ap.parse_args()

    pack = args.pack.resolve()
    manifest = json.loads((pack / "pack.json").read_text())
    presets = [p for p in manifest["presets"] if p["kind"] == "transition"]
    if not presets:
        raise SystemExit(f"{pack}: no transition presets to preview")
    print(f"{manifest['id']}: {len(presets)} transition preset(s)")

    transitions = [{"name": p["id"], "id": "foxfire_transition",
                    "settings": {"pack": manifest["id"], "preset": p["id"]}} for p in presets]

    scratch = Path(tempfile.mkdtemp(prefix="ff-preview-src-"))
    rec_dir = scratch / "rec"
    rec_dir.mkdir()
    cfg = Path(tempfile.mkdtemp(prefix="ff-preview-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    timeline: list = []
    try:
        proof.write_ws_config(obs_cfg)
        proof.install_plugin(Path(args.plugin_build).resolve(), obs_cfg)
        proof.install_pack(pack, obs_cfg)
        # Same reasoning as transition-proof.py's: a dev build's public key is all zeros, so a
        # paid pack cannot verify there and would render nothing. This edits the throwaway copy
        # inside this run's own temporary OBS config, never the pack.
        tps.unlicense_installed_copy(obs_cfg, manifest["id"])
        write_collection(obs_cfg, transitions)
        proof.wait_for_port_free(proof.PORT)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi",
             "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            asyncio.run(drive(rec_dir, presets, timeline))
        finally:
            proof.terminate_process_group(p)

        recs = sorted(rec_dir.glob("*.mkv"))
        if not recs:
            raise SystemExit("OBS wrote no recording")
        rec = recs[-1]
        out = args.out.resolve()
        (out / "frames").mkdir(parents=True, exist_ok=True)
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(rec),
                        "-c:v", "libx264", "-preset", "medium", "-crf", "20", "-an",
                        str(out / (manifest["id"] + "-all.mp4"))], check=True)

        sheets = {"early": [], "mid": []}
        for t in timeline:
            for when in sheets:
                f = tps.frame_at(rec, t[when])
                f.save(out / "frames" / f"{t['id']}-{when}.png")
                sheets[when].append(label(f.resize((320, 180)), t["name"]))
        for when, tiles in sheets.items():
            if not tiles:
                continue
            cols = 4 if len(tiles) > 6 else 3
            rows = (len(tiles) + cols - 1) // cols
            sheet = Image.new("RGB", (cols * 320, rows * 180), (18, 18, 20))
            for i, tile in enumerate(tiles):
                sheet.paste(tile, ((i % cols) * 320, (i // cols) * 180))
            sheet.save(out / f"{manifest['id']}-sheet-{when}.png")

        print(f"\n{len(timeline)} preset(s) recorded")
        pid = manifest["id"]
        print(f"  video: {out / (pid + '-all.mp4')}")
        print(f"  sheets: {out / (pid + '-sheet-early.png')} (edge mid-travel)")
        print(f"          {out / (pid + '-sheet-mid.png')} (at the cut)")
        return 0
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
