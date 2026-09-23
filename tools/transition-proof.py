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
SKIPS: list[str] = []

A_RGB = (220, 40, 40)
B_RGB = (40, 60, 220)
DURATION_MS = 4000

# The packs live in a separate private repo. When it is not checked out, the wipe half of this
# proof cannot run -- and says so, rather than passing with one fewer check and the same exit 0.
# Two default locations because there are two layouts: CI checks the packs repo out beside this
# one, and on junkie's machine both live under ~/vault/projects.
def _find_packs() -> Path:
    here = Path(__file__).resolve().parent.parent
    for c in ([Path(os.environ["FF_PACKS"])] if os.environ.get("FF_PACKS") else []) + [
            here.parent / "foxfire" / "packs",
            Path.home() / "vault" / "projects" / "foxfire" / "packs"]:
        if (c / "basics").is_dir():
            return c
    return here.parent / "foxfire" / "packs"


WIPE_PACK = _find_packs() / "basics"
WIPE_PRESET = "wipe-linear"
WIPE_SOFTNESS = 0.15
SWEEP_PRESET = "sweep-liquid"


def check(name, ok, detail):
    CHECKS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILS.append(name)


def skip(name, why):
    SKIPS.append(name)
    print(f"  [SKIP] {name}: {why}")


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
        # The plugin transitions, seeded here because obs-websocket cannot create one. Two of
        # them, selected in turn inside one recording: the bundled dissolve, which needs no pack
        # installed, and the basics pack's mask-driven wipe, whose whole point is that the shape
        # comes from a PNG and which therefore cannot be checked by "is this frame a mix".
        "transitions": [{"name": "FF", "id": "foxfire_transition",
                         "settings": {"pack": "demo", "preset": "dissolve"}},
                        {"name": "FFW", "id": "foxfire_transition",
                         "settings": {"pack": "basics", "preset": WIPE_PRESET}},
                        {"name": "FFS", "id": "foxfire_transition",
                         "settings": {"pack": "basics", "preset": SWEEP_PRESET}}],
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


def load_sweep_generator():
    """tools/make-sweep-thumbs.py out of the packs repo, or None when it is not checked out."""
    import importlib.util
    gen = WIPE_PACK.parent.parent / "tools" / "make-sweep-thumbs.py"
    if not gen.is_file():
        return None
    spec = importlib.util.spec_from_file_location("make_sweep_thumbs", gen)
    if not spec or not spec.loader:
        return None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_generator():
    """tools/make-wipe-masks.py out of the packs repo, or None when it is not checked out.

    Imported rather than reimplemented: the point of the check below is that the numpy `wipe()`
    those thumbnails are drawn with and the HLSL one OBS runs are the same function. A second
    copy written here would agree with neither.
    """
    import importlib.util
    gen = WIPE_PACK.parent.parent / "tools" / "make-wipe-masks.py"
    if not gen.is_file():
        return None
    spec = importlib.util.spec_from_file_location("make_wipe_masks", gen)
    if not spec or not spec.loader:
        return None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def coverage_profile(img: Image.Image, out_rgb, in_rgb):
    """Per column, how far that column has travelled from the outgoing scene to the incoming one.

    Averaged down the column because the linear mask is constant in y, so noise from the video
    encoder averages out while the wipe's shape does not.
    """
    import numpy as np
    a = np.asarray(img, dtype=float)
    span = np.array(in_rgb, float) - np.array(out_rgb, float)
    # project each pixel onto the A->B line; the two colours differ in every channel, so this is
    # far steadier than reading one of them
    cov = ((a - np.array(out_rgb, float)) @ span) / float(span @ span)
    return cov.mean(axis=0)


def mask_row(png: Path, columns: int):
    """The shipped mask PNG, sampled at the canvas's column centres exactly as `fit = 0` does."""
    import numpy as np
    m = np.asarray(Image.open(png).convert("L"), dtype=float) / 255.0
    src = (np.arange(m.shape[1]) + 0.5) / m.shape[1]
    dst = (np.arange(columns) + 0.5) / columns
    return np.interp(dst, src, m.mean(axis=0))


def fit_progress(mw, observed, mask, softness):
    """The progress value that best explains the recorded frame, and how well it does.

    A fit rather than an assertion about a fixed moment: the frame comes out of a video file at a
    requested timestamp, so which instant of the transition it caught is known only to within a
    frame or two. What is being measured is the SHAPE -- that the frame is this mask at some point
    in its travel -- and the residual is what says so.
    """
    import numpy as np
    zero = np.zeros(mask.shape + (1,))
    one = np.ones(mask.shape + (1,))
    best = (1e9, 0.0)
    for t in np.arange(0.0, 1.0001, 0.002):
        pred = mw.wipe(zero, one, mask, float(t), softness)[:, 0]
        rms = float(np.sqrt(np.mean((pred - observed) ** 2)))
        best = min(best, (rms, float(t)))
    return best


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

        secs = DURATION_MS / 1000.0
        wipe_ok = "FFW" in names and WIPE_PACK.is_dir()

        await start_recording(c)
        await asyncio.sleep(2.0)                    # settled on A
        await c.request("SetCurrentProgramScene", {"sceneName": "B"})
        await asyncio.sleep(secs + 2.0)             # through the cut and settled on B
        # The second cut, B back to A, with the mask-driven wipe selected. In the same recording:
        # booting OBS twice to measure two transitions would double the slowest part of the run.
        wipe_mid = 2.0 + secs + 2.0 + secs / 2.0
        sweep_ok = "FFS" in names and WIPE_PACK.is_dir()
        sweep_mid = wipe_mid + secs / 2.0 + 2.0 + secs / 2.0
        if wipe_ok:
            await c.request("SetCurrentSceneTransition", {"transitionName": "FFW"})
            await c.request("SetCurrentSceneTransitionDuration", {"transitionDuration": DURATION_MS})
            await c.request("SetCurrentProgramScene", {"sceneName": "A"})
            await asyncio.sleep(secs + 2.0)
        # The third cut, A to B again, with the covering sweep. Its whole claim is that the middle
        # of the cut shows NEITHER scene, which no other check in this file can see.
        if sweep_ok:
            await c.request("SetCurrentSceneTransition", {"transitionName": "FFS"})
            await c.request("SetCurrentSceneTransitionDuration", {"transitionDuration": DURATION_MS})
            await c.request("SetCurrentProgramScene", {"sceneName": "B"})
            await asyncio.sleep(secs + 2.0)
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

        # --- the mask-driven wipe -------------------------------------------------------------
        # Everything above would pass on a transition that ignored its mask entirely: a uniform
        # blend is "neither scene" and "about halfway" too. What separates a wipe from a dissolve
        # is that the frame has SHAPE, and the shape is the PNG.
        mw = load_generator()
        if not wipe_ok:
            skip("the basics pack's mask-driven wipe renders its mask",
                 f"'FFW' in OBS's list: {'FFW' in names}; pack dir {WIPE_PACK}: {WIPE_PACK.is_dir()} "
                 f"-- set FF_PACKS to the packs repo's packs/ to run this half")
        elif mw is None:
            skip("the basics pack's mask-driven wipe renders its mask",
                 "tools/make-wipe-masks.py not found next to the pack; nothing to compare against")
        else:
            import numpy as np
            frame = frame_at(path, wipe_mid)
            # B is outgoing this time and A incoming -- the second cut runs the other way
            observed = coverage_profile(frame, B_RGB, A_RGB)
            # the mask the PRESET selects, read from the manifest rather than guessed from its id
            manifest = json.loads((WIPE_PACK / "pack.json").read_text())
            preset = next(p for p in manifest["presets"] if p["id"] == WIPE_PRESET)
            mask = mask_row(WIPE_PACK / preset["layers"][0]["params"]["mask"], frame.size[0])
            rms, t = fit_progress(mw, observed, mask, WIPE_SOFTNESS)
            # Measured, by pinning the shader's luma to a constant: an uncovered frame fits
            # progress 0.000 with an RMS of 0.0034. A flat profile is not unfittable, it is
            # PERFECTLY fittable at either end, where every mask predicts the same flat frame.
            # So the residual only means something inside the travel, and the shape check says so
            # rather than leaving a green line next to a transition that ignores its mask.
            in_flight = 0.15 < t < 0.85
            check("the wipe was caught in flight, not at either end",
                  in_flight,
                  f"best-fit progress {t:.3f} -- at 0 or 1 the frame is flat and EVERY mask fits "
                  f"it exactly, so a residual measured there says nothing about the shape")
            check("the recorded frame is this mask, at some point in its travel",
                  in_flight and rms < 0.06,
                  f"RMS {rms:.4f} against make-wipe-masks.py's own wipe() at progress {t:.3f}"
                  + ("" if in_flight else " -- REJECTED: that is an endpoint, see above"))
            left = float(observed[: frame.size[0] // 4].mean())
            right = float(observed[-frame.size[0] // 4:].mean())
            check("and it travels the way the mask's greys run, left to right",
                  left - right > 0.35,
                  f"left quarter {left:.2f} vs right quarter {right:.2f} -- equal means no wipe, "
                  f"reversed means the mask is being read inverted")
            np.save("/tmp/ff-wipe-profile.npy", observed)
            frame.save("/tmp/ff-wipe-mid.png")
            print("  wipe frame: /tmp/ff-wipe-mid.png")

        # --- the covering sweep ----------------------------------------------------------------
        # A sweep is not judged by its shape but by what it HIDES. In the middle of the cut the
        # frame must contain no trace of either scene: that is the one thing a bought video
        # stinger does that a blend cannot, and the reason this preset exists.
        msw = load_sweep_generator()
        if not sweep_ok or msw is None:
            skip("the covering sweep hides the cut completely",
                 f"'FFS' in OBS's list: {'FFS' in names}; generator found: {msw is not None}; "
                 f"pack dir {WIPE_PACK}")
        else:
            import numpy as np
            sframe = frame_at(path, sweep_mid)
            got = np.asarray(sframe, dtype=float)
            near_a = (np.abs(got - np.array(A_RGB, float)).max(axis=2) < 24).sum()
            near_b = (np.abs(got - np.array(B_RGB, float)).max(axis=2) < 24).sum()
            total = got.shape[0] * got.shape[1]
            check("mid-sweep the frame keeps no trace of either scene",
                  (near_a + near_b) / total < 0.005,
                  f"{near_a} px near A and {near_b} px near B out of {total} "
                  f"({100.0 * (near_a + near_b) / total:.2f}%) -- anything above a rounding "
                  f"fraction means the cut is visible through the cover")
            pr = msw.SWEEPS[SWEEP_PRESET]
            # Fitted across the hold, not assumed at its centre. The fill DRIFTS as the sweep
            # travels, so the frame is a function of progress even while coverage is complete, and
            # the recording's own start latency put the captured frame at about 0.575 of a cut
            # requested at 0.5 -- a 12px offset that read as a total mismatch (RMS 0.19) until it
            # was aligned (0.03). Same treatment as the wipe above: measure the shape, fit the
            # moment.
            blank = np.zeros((sframe.size[1], sframe.size[0], 3))
            lo, hi = pr["swap_point"] - pr["hold"] / 2, pr["swap_point"] + pr["hold"] / 2
            best = (1e9, 0.0)
            for t in np.linspace(lo, hi, 21):
                want = msw.render(WIPE_PACK / "art", pr, float(t), sframe.size, blank).astype(float)
                best = min(best, (float(np.sqrt(np.mean(((got - want) / 255.0) ** 2))), float(t)))
            rms, at = best
            check("and it is the fill art the preset asks for",
                  rms < 0.06,
                  f"RMS {rms:.4f} against make-sweep-thumbs.py's own render of this preset, best "
                  f"fit at progress {at:.3f} -- the scene colours are flat, so a frame that failed "
                  f"to cover would be nowhere near it at any progress")
            want = msw.render(WIPE_PACK / "art", pr, at, sframe.size, blank).astype(float)
            sframe.save("/tmp/ff-sweep-mid.png")
            Image.fromarray(want.clip(0, 255).astype(np.uint8), "RGB").save("/tmp/ff-sweep-want.png")
            print("  sweep frame: /tmp/ff-sweep-mid.png  (predicted: /tmp/ff-sweep-want.png)")
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
        if WIPE_PACK.is_dir():
            proof.install_pack(WIPE_PACK, obs_cfg)
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

    print(f"\ntransition proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed"
          + (f", {len(SKIPS)} SKIPPED ({', '.join(SKIPS)})" if SKIPS else ""))
    if not CHECKS:
        print("transition proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
