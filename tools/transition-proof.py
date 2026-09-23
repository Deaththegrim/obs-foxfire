#!/usr/bin/env python3
"""The scene transition, driven for real.

A transition renders nothing in a still screenshot of an idle OBS: it exists only during a cut.
So this builds a scene collection with two scenes of known, deliberately different colours, makes
the Foxfire transition the active one, sets a long duration, switches scenes, and screenshots
while it is in flight. The frame in the middle must be NEITHER scene -- a transition that silently
fell back to a cut, or rendered its `image` builtin instead of tex_a/tex_b, shows up here and
nowhere else.

The audio half is the one that matters most and is the easiest to get wrong. libobs does not mix
the outgoing and incoming scenes for a transition; if audio_render is missing or wrong the scene
goes SILENT for the duration, which no pixel gate can see. This records the program output across
a cut between two scenes that are both playing a tone, and measures that the sound does not
disappear in the middle -- the failure an equal-gain crossfade produces as a dip and a missing
audio_render produces as a hole.

A transition cannot be created over obs-websocket -- OBS owns the transition list -- so the scene
collection is written to disk before OBS starts.
"""
import argparse
import asyncio
import json
import os
import shutil
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

FAILS: list[str] = []
CHECKS: list[str] = []

A_RGB = (220, 40, 40)
B_RGB = (40, 60, 220)
DURATION_MS = 4000


def check(name, ok, detail):
    CHECKS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILS.append(name)


def bgra(rgb) -> int:
    """OBS colour_source stores ABGR as an int."""
    r, g, b = rgb
    return (0xFF << 24) | (b << 16) | (g << 8) | r


def scene_collection(name: str, tone_a: Path, tone_b: Path) -> dict:
    """Two scenes, each a flat colour, and the Foxfire transition already selected.

    Flat colours on purpose: the whole measurement is "is this frame a mix of the two", and a
    scene with detail in it would make that a judgement call instead of an arithmetic one.
    """
    def colour(src_name, rgb):
        return {"name": src_name, "id": "color_source_v3", "versioned_id": "color_source_v3",
                "settings": {"color": bgra(rgb), "width": 640, "height": 360},
                "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
                "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
                "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
                "deinterlace_field_order": 0, "monitoring_type": 0, "private_settings": {}}

    def media(src_name, wav):
        """A tone in BOTH scenes, at DIFFERENT frequencies.

        Both, so a level that holds across the cut means the crossfade kept it -- with sound in
        only one scene, "audio survived" and "audio arrived" would be the same measurement.

        Different, because two sines at the SAME frequency from two unsynchronised media sources
        meet at an arbitrary relative phase, and the sum is anywhere between 2x and nothing. That
        fixture measured phase, not the crossfade: the identical-tone version of this read 1.41
        on one run and 0.45 on the next off the same code. Uncorrelated signals add in POWER, so
        an equal-power crossfade holds RMS flat and an equal-gain one dips to 0.71 -- both
        arithmetic, neither luck.
        """
        return {"name": src_name, "id": "ffmpeg_source", "versioned_id": "ffmpeg_source",
                "settings": {"local_file": str(wav), "looping": True, "is_local_file": True},
                "mixers": 255, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
                "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
                "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
                "deinterlace_field_order": 0, "monitoring_type": 0, "private_settings": {}}

    def item(child, item_id):
        return {"name": child, "source_uuid": "", "visible": True, "locked": False,
                "rot": 0.0, "pos": {"x": 0.0, "y": 0.0}, "scale": {"x": 1.0, "y": 1.0},
                "align": 5, "bounds_type": 0, "bounds_align": 0, "bounds": {"x": 0.0, "y": 0.0},
                "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0,
                "id": item_id, "group_item_backup": False, "scale_filter": "disable",
                "blend_method": "default", "blend_type": "normal",
                "show_transition": {"duration": 0}, "hide_transition": {"duration": 0},
                "private_settings": {}}

    def scene(scene_name, child):
        return {"name": scene_name, "id": "scene", "versioned_id": "scene",
                "settings": {"id_counter": 3, "custom_size": False,
                             "items": [item(child, 1), item("tone" + scene_name, 2)]},
                "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
                "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
                "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {},
                "deinterlace_mode": 0, "deinterlace_field_order": 0, "monitoring_type": 0,
                "private_settings": {}}

    return {
        "name": name,
        "current_scene": "A",
        "current_program_scene": "A",
        "current_transition": "FF",
        "transition_duration": DURATION_MS,
        "scene_order": [{"name": "A"}, {"name": "B"}],
        "sources": [colour("solidA", A_RGB), colour("solidB", B_RGB),
                    media("toneA", tone_a), media("toneB", tone_b),
                    scene("A", "solidA"), scene("B", "solidB")],
        # The plugin transition, seeded here because obs-websocket cannot create one.
        "transitions": [{"name": "FF", "id": "foxfire_transition",
                         "settings": {"pack": "demo", "preset": "dissolve"}}],
        "groups": [], "quick_transitions": [], "saved_projectors": [], "canvases": [],
        "preview_locked": False, "scaling_enabled": False, "scaling_level": 0,
        "scaling_off_x": 0.0, "scaling_off_y": 0.0, "virtual-camera": {"type2": 3},
        "resolution": {"x": 640, "y": 360}, "version": 2, "modules": {},
    }


def write_collection(obs_cfg: Path, tone_a: Path, tone_b: Path) -> None:
    d = obs_cfg / "basic" / "scenes"
    d.mkdir(parents=True, exist_ok=True)
    (d / "Untitled.json").write_text(json.dumps(scene_collection("Untitled", tone_a, tone_b), indent=4))
    p = obs_cfg / "basic" / "profiles" / "Untitled"
    p.mkdir(parents=True, exist_ok=True)
    (p / "basic.ini").write_text(
        "[General]\nName=Untitled\n"
        "[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSCommon=30\n"
        "[Output]\nMode=Simple\n[SimpleOutput]\nRecFormat2=mkv\nRecQuality=Small\nRecEncoder=x264\n")
    (obs_cfg / "global.ini").write_text(
        "[Basic]\nProfile=Untitled\nProfileDir=Untitled\nSceneCollection=Untitled\nSceneCollectionFile=Untitled\n")


def tone_wav(path: Path, seconds=6.0, hz=440.0, rate=48000, amp=0.5):
    import math
    import struct
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(amp * 32767 * math.sin(2 * math.pi * hz * i / rate)))
                               for i in range(int(seconds * rate))))


async def wait_recording_stopped(c, timeout: float = 30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not (await c.request("GetRecordStatus")).get("outputActive", False):
            return
        await asyncio.sleep(0.2)
    raise RuntimeError(f"the recording was still running {timeout}s after StopRecord")


async def start_recording(c, timeout: float = 30.0):
    await c.request("StartRecord")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if (await c.request("GetRecordStatus")).get("outputActive", False):
            return
        await asyncio.sleep(0.2)
    raise RuntimeError(f"the recording never started within {timeout}s")


def frame_at(path: Path, seconds: float) -> Image.Image:
    """One frame of the RECORDING, which is the program output.

    Not GetSourceScreenshot: that renders a named source on its own, and a transition is not in
    the canvas by name -- OBS refuses it with "No source was found by the name of `FF`". The
    transition exists only as the composite between two scenes, so the program output is the only
    place it can be observed at all.
    """
    out = path.with_suffix(f".{seconds:.2f}.png")
    r = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{seconds}", "-i", str(path),
                        "-frames:v", "1", "-y", str(out)], capture_output=True)
    if not out.exists():
        raise RuntimeError(f"no frame at {seconds}s of {path.name}: "
                           f"{r.stderr.decode(errors='replace')[:300]}")
    return Image.open(out).convert("RGB")


def audio_rms(path: Path, start: float, length: float) -> float:
    """RMS of a slice of the recording's audio, 0..1, decoded to raw s16 so nothing depends on
    parsing ffmpeg's human-readable output."""
    r = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{start}", "-t", f"{length}", "-i", str(path),
                        "-vn", "-ac", "1", "-ar", "16000", "-f", "s16le", "-"], capture_output=True)
    raw = r.stdout
    if not raw:
        raise RuntimeError(f"no audio decoded from {path.name} at {start}s: "
                           f"{r.stderr.decode(errors='replace')[:300]}")
    import array
    a = array.array("h")
    a.frombytes(raw[: len(raw) // 2 * 2])
    if not len(a):
        return 0.0
    return (sum(float(v) * v for v in a) / len(a)) ** 0.5 / 32768.0


def mean_rgb(img: Image.Image):
    px = img.load()
    w, h = img.size
    tot = [0.0, 0.0, 0.0]
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            c = px[x, y]
            for i in range(3):
                tot[i] += c[i]
    n = len(range(0, h, 4)) * len(range(0, w, 4))
    return tuple(t / n for t in tot)


def dist(a, b) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


async def drive(rec_dir: Path):
    ws, c = await ff_proof.open_client("transition proof")
    try:
        names = [t["transitionName"] for t in (await c.request("GetSceneTransitionList"))["transitions"]]
        check("the Foxfire transition is in OBS's transition list",
              "FF" in names,
              f"transitions={names} -- a transition OBS never registered cannot be selected, and "
              f"every check below would silently measure the built-in Fade instead")
        if "FF" not in names:
            return

        await c.request("SetCurrentSceneTransition", {"transitionName": "FF"})
        await c.request("SetCurrentSceneTransitionDuration", {"transitionDuration": DURATION_MS})
        cur = await c.request("GetCurrentSceneTransition")
        check("it is the active transition and reports its own kind",
              cur["transitionName"] == "FF" and cur["transitionKind"] == "foxfire_transition",
              f"name={cur['transitionName']} kind={cur['transitionKind']}")

        await c.request("SetCurrentProgramScene", {"sceneName": "A"})
        await asyncio.sleep(DURATION_MS / 1000.0 + 0.5)
        await c.request("SetRecordDirectory", {"recordDirectory": str(rec_dir)})

        await start_recording(c)
        await asyncio.sleep(2.0)                    # settled on A
        await c.request("SetCurrentProgramScene", {"sceneName": "B"})
        await asyncio.sleep(DURATION_MS / 1000.0 + 2.0)   # through the cut and settled on B
        path = Path((await c.request("StopRecord"))["outputPath"])
        await wait_recording_stopped(c)

        half = DURATION_MS / 2000.0
        before = mean_rgb(frame_at(path, 1.0))
        mid = mean_rgb(frame_at(path, 2.0 + half))
        after = mean_rgb(frame_at(path, 2.0 + DURATION_MS / 1000.0 + 1.0))

        check("before the cut the output is scene A",
              dist(before, A_RGB) < 45,
              f"mean={tuple(round(x) for x in before)} vs A {A_RGB}")
        check("mid-transition the frame is NEITHER scene",
              dist(mid, A_RGB) > 45 and dist(mid, B_RGB) > 45,
              f"mean={tuple(round(x) for x in mid)}; A={A_RGB} B={B_RGB} -- a cut, or a shader "
              f"reading `image` instead of tex_a/tex_b, lands on one of them exactly")
        expect = tuple((a + b) / 2 for a, b in zip(A_RGB, B_RGB))
        check("and it is about halfway between them",
              dist(mid, expect) < 70,
              f"mean={tuple(round(x) for x in mid)} vs the midpoint {tuple(round(x) for x in expect)}")
        check("after the cut the output is scene B",
              dist(after, B_RGB) < 45,
              f"mean={tuple(round(x) for x in after)} vs B {B_RGB}")

        # The half libobs will not do for us. Both scenes carry the same tone, so a correct
        # crossfade holds the level roughly flat across the cut; a missing audio_render drops it
        # to silence for the whole duration and no pixel above would notice.
        quiet_ref = audio_rms(path, 0.5, 1.0)
        during = audio_rms(path, 2.0 + half - 0.4, 0.8)
        check("the audio does not disappear across the cut",
              during > quiet_ref * 0.4,
              f"RMS {during:.5f} mid-transition against {quiet_ref:.5f} before it -- a transition "
              f"with no audio_render renders the scene SILENT for its whole duration")
        check("and it does not dip like an equal-gain crossfade",
              during > quiet_ref * 0.85,
              f"mid/before = {during / max(quiet_ref, 1e-9):.2f}; summing two uncorrelated signals "
              f"at (1-t) and t sinks to about 0.71 in the middle, which is the hole this uses "
              f"sqrt to avoid")
    finally:
        await ws.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    args = ap.parse_args()
    repo = Path(args.plugin_build).resolve()
    scratch = Path(tempfile.mkdtemp(prefix="ff-trans-src-"))
    tone_a = scratch / "tone-a.wav"
    tone_b = scratch / "tone-b.wav"
    tone_wav(tone_a, hz=440.0)
    tone_wav(tone_b, hz=660.0)
    rec_dir = scratch / "rec"
    rec_dir.mkdir()
    cfg = Path(tempfile.mkdtemp(prefix="ff-trans-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    try:
        proof.write_ws_config(obs_cfg)
        proof.install_plugin(repo, obs_cfg)
        write_collection(obs_cfg, tone_a, tone_b)
        proof.wait_for_port_free(proof.PORT)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi", "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            asyncio.run(drive(rec_dir))
        finally:
            proof.terminate_process_group(p)
            logs = sorted(obs_cfg.glob("logs/*.txt"))
            if logs:
                shutil.copy(logs[-1], "/tmp/ff-transition-obs.log")
                print("log: /tmp/ff-transition-obs.log")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    print(f"\ntransition proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("transition proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
