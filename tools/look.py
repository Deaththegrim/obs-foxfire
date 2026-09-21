#!/usr/bin/env python3
"""Look-dev renderer for Foxfire packs: what does this actually LOOK like, and does it move?

This is NOT a gate. It asserts nothing and it never decides a preset is good or bad -- that call
is the artist's, and the whole point is to put the pixels in front of them. What it does is boot
the same headless sandbox tools/proof.py uses (real OBS under Xvfb, real plugin, real pack), drive
one preset at a time through a sweep of audio states, and hand back:

  * contact-sheet.png -- every preset x every audio state in one grid, on a checkerboard so
    transparent reads as transparent instead of as black
  * frames/<preset>-<state>-<n>.png -- the individual frames, full size
  * motion/<preset>.gif -- a short animated capture, because a still cannot tell you whether the
    movement reads

It does report its own coverage, because a look tool that quietly renders nothing looks exactly
like a look tool that rendered something dark: the summary names how many presets and states were
inspected, and flags a frame that came back empty rather than showing you a black square and
letting you assume it is art.

It also reports, per preset and state, the SPREAD of non-blank pixels across the frames it took.
A spread of zero means nothing changed between shots -- the preset is frozen, not reacting -- and
that is worth seeing even though it is not always wrong (a static overlay is allowed to be static).

Usage:
    tools/look.py --plugin-build build_x86_64 --pack data/packs/demo --out build_x86_64/look
    cshow build_x86_64/look/contact-sheet.png
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import io
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ff_proof  # noqa: E402  -- Client/open_client/PORT; sys.path set just above
import proof  # noqa: E402  -- sandbox boot helpers, reused rather than re-authored

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

W, H = 640, 360
RATE = 48000
SETTLE = 1.6          # seconds after a settings change before the analyser's envelope has caught up
FRAME_GAP = 0.45      # seconds between the stills of one state
MOTION_FRAMES = 24
MOTION_GAP = 0.12
MOTION_STATE = "beat 120bpm"  # the state the per-preset GIF is captured under

VISUALIZER_KINDS = proof.VISUALIZER_KINDS
EFFECTS_KINDS = proof.EFFECTS_KINDS


# --------------------------------------------------------------------------- audio states

def _write_wav(path: Path, samples) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767))
                               for s in samples))


def silence(path: Path, seconds: float = 2.0) -> None:
    """Actual digital silence, fed through the same tap as every other state.

    Found by running this tool: switching the source's audio_mode to 0 (master mix) does NOT give
    a silent frame here, because the 'tone' media source loops from the moment it is created and
    is itself on the master mix. Silence and a 110 Hz tone measured 0.1074 and 0.1073 core -- the
    same frame twice. Feeding a silent file keeps one audio path for all seven states instead of a
    special case that quietly measured the wrong thing."""
    _write_wav(path, (0.0 for _ in range(int(RATE * seconds))))


def tone(path: Path, hz: float, seconds: float = 2.0, amp: float = 0.5) -> None:
    _write_wav(path, (amp * math.sin(2 * math.pi * hz * i / RATE)
                      for i in range(int(RATE * seconds))))


def noise(path: Path, seconds: float = 2.0, amp: float = 0.35) -> None:
    rng = random.Random(1234)  # seeded: two runs of the same pack should be comparable
    _write_wav(path, (amp * (rng.random() * 2.0 - 1.0) for _ in range(int(RATE * seconds))))


def beat(path: Path, seconds: float = 2.0, bpm: float = 120.0, amp: float = 0.9) -> None:
    """Broadband hits on the beat, each with a sharp attack and a fast decay.

    Steady sine gives the beat detector nothing: spectral flux measures CHANGE, and a constant
    tone has none after its first frame. Onsets are what make the beat counter tick, so a preset
    that flashes on the beat only shows that behaviour under this state."""
    rng = random.Random(99)
    period = int(RATE * 60.0 / bpm)
    decay = period * 0.25

    def gen():
        for i in range(int(RATE * seconds)):
            t = i % period
            env = math.exp(-t / decay)
            yield amp * env * (rng.random() * 2.0 - 1.0)

    _write_wav(path, gen())


def swell(path: Path, hz: float = 220.0, seconds: float = 2.0, amp: float = 0.9) -> None:
    """A tone ramping silent -> loud, so an envelope-driven preset shows its whole range."""
    n = int(RATE * seconds)
    _write_wav(path, (amp * (i / n) * math.sin(2 * math.pi * hz * i / RATE) for i in range(n)))


# state name -> (builder or None for silence, one-line note shown under the column)
Builder = Callable[[Path], None]
STATES: list[tuple[str, Builder, str]] = [
    ("silence", lambda p: silence(p), "no signal at all"),
    ("bass 110Hz", lambda p: tone(p, 110.0), "low band only"),
    ("mid 1kHz", lambda p: tone(p, 1000.0), "mid band only"),
    ("treble 6kHz", lambda p: tone(p, 6000.0), "high band only"),
    ("full noise", lambda p: noise(p), "every band at once"),
    ("beat 120bpm", lambda p: beat(p), "onsets -- drives beat"),
    ("swell", lambda p: swell(p), "quiet -> loud ramp"),
]


# --------------------------------------------------------------------------- frame handling

def _decode(b64: str) -> Image.Image:
    return Image.open(io.BytesIO(base64.b64decode(b64.split(",", 1)[1]))).convert("RGBA")


async def shoot(client, source_name: str) -> Image.Image:
    r = await client.request("GetSourceScreenshot", {
        "sourceName": source_name, "imageFormat": "png",
        "imageWidth": W, "imageHeight": H})
    return _decode(r["imageData"])


def metrics(img: Image.Image) -> dict:
    """The three units tools/ff_proof.py's Shot class arrived at, plus mean luminance.

    Learned by running this tool and getting a useless answer: a single alpha>8 ratio cannot tell
    a silent frame from a loud one on any pack with a glow layer, because the glow smears low
    alpha across the whole frame -- 'bars' measured 0.1481 in silence and 0.1479 on a 110 Hz tone.
    core (alpha>200) is the unit that tracks the BARS; lit is the unit that tracks the halo.

    luma is for the effects kind, where alpha is useless for the opposite reason: a filter runs on
    an opaque stand-in source, so every pixel is alpha 255 and every alpha unit reads 1.0000 in
    every state. What moves there is brightness, so that is what gets measured."""
    alpha = img.getchannel("A").tobytes()
    n = float(len(alpha))
    grey = img.convert("L").tobytes()
    return {
        "core": sum(1 for a in alpha if a > 200) / n,
        "lit": sum(1 for a in alpha if a > 8) / n,
        "luma": sum(grey) / (255.0 * len(grey)),
    }


def checkerboard(size: tuple[int, int], square: int = 12) -> Image.Image:
    """The backdrop for the sheet. The visualizer renders WITH alpha and is meant to sit over a
    stream; composited onto black, 'transparent' and 'black' are the same pixel and you cannot
    review the difference. On a checkerboard they are obviously different."""
    w, h = size
    bg = Image.new("RGBA", size, (58, 58, 64, 255))
    d = ImageDraw.Draw(bg)
    for y in range(0, h, square):
        for x in range(0, w, square):
            if (x // square + y // square) % 2:
                d.rectangle([x, y, x + square - 1, y + square - 1], fill=(78, 78, 86, 255))
    return bg


def over_checker(img: Image.Image) -> Image.Image:
    bg = checkerboard(img.size)
    bg.alpha_composite(img)
    return bg


def _fit(d: ImageDraw.ImageDraw, text: str, font, width: int) -> str:
    """Shorten text with an ellipsis until it fits `width`.

    A label that runs past its column does not wrap -- it is painted over whatever is beside it,
    and the reader sees a sentence that stops mid-word with no sign anything was lost. Measuring
    is the only way the sheet can be trusted to say what it means."""
    if d.textlength(text, font=font) <= width:
        return text
    while text and d.textlength(text + "...", font=font) > width:
        text = text[:-1]
    return text + "..."


def _font(size: int):
    for candidate in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        if Path(candidate).exists():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


# --------------------------------------------------------------------------- driving OBS

def state_slug(state: str) -> str:
    return state.replace(" ", "-")


def tap_name(state: str) -> str:
    return f"tone-{state_slug(state)}"


async def create_taps(client, scene: str, tmp: Path) -> None:
    """One looping media source per audio state, each with its own file, created once.

    The first design wrote every state to a single WAV and forced a reload by blanking local_file
    and setting it back. Measured, that did not work: after the first reload the source stopped
    producing audio and all seven states rendered one identical frozen frame (core 0.2172 across
    the board). A source per state means OBS opens each file once and nothing ever has to be
    convinced to re-read anything -- switching states is just re-pointing the visualizer's tap."""
    for state, builder, _note in STATES:
        wav = tmp / f"{state_slug(state)}.wav"
        builder(wav)
        await client.request("CreateInput", {
            "sceneName": scene, "inputName": tap_name(state), "inputKind": "ffmpeg_source",
            "inputSettings": {"local_file": str(wav), "is_local_file": True, "looping": True},
            "sceneItemEnabled": True})
    await asyncio.sleep(1.5)


async def _set_audio(client, name: str, kind: str, settings: dict) -> None:
    """A visualizer carries its audio settings on the input; an effects preset carries them on the
    filter attached to the stand-in source. One seam, so the two paths cannot drift."""
    if kind in EFFECTS_KINDS:
        await client.request("SetSourceFilterSettings", {
            "sourceName": name, "filterName": "fx", "filterSettings": settings})
    else:
        await client.request("SetInputSettings", {"inputName": name, "inputSettings": settings})


async def capture_preset(client, pack_id: str, preset: dict, out: Path,
                         frames: int, motion: bool, report: dict) -> None:
    """One preset through every audio state. Rows are appended to report as they are produced, so
    a preset that dies partway through still leaves behind what it did render."""
    pid = preset["id"]
    kind = preset["kind"]
    name = f"look-{pack_id}-{pid}"
    settings = {"pack": pack_id, "preset": pid, "audio_mode": 1,
                "audio_source": tap_name(STATES[0][0])}
    baseline = 0.0
    unit = "diff" if kind in EFFECTS_KINDS else "core"

    if kind in EFFECTS_KINDS:
        # a flat grey stand-in to filter: glow-only is `c + blur(c)*amount`, so on a flat colour
        # the filter's effect is visible as a brightness change rather than as shape
        await client.request("CreateInput", {
            "sceneName": "fflook", "inputName": name, "inputKind": "color_source_v3",
            "inputSettings": {"color": 0xFF404040, "width": W, "height": H}})
        await asyncio.sleep(1.0)
        # the unfiltered stand-in, so every later frame can be reported as a CHANGE the filter
        # made rather than as an absolute number that is the same whatever the filter does
        baseline = metrics(await shoot(client, name))["luma"]
        await client.request("CreateSourceFilter", {
            "sourceName": name, "filterName": "fx", "filterKind": "foxfire_effects",
            "filterSettings": settings})
    elif kind in VISUALIZER_KINDS:
        await client.request("CreateInput", {
            "sceneName": "fflook", "inputName": name, "inputKind": "foxfire_visualizer",
            "inputSettings": dict(settings, width=W, height=H)})
    else:
        report["skipped"].append({"pack": pack_id, "id": pid, "kind": kind,
                                  "why": "unrecognised kind -- not rendered"})
        return

    for state, _builder, _note in STATES:
        slug = state_slug(state)
        await _set_audio(client, name, kind,
                         {"audio_mode": 1, "audio_source": tap_name(state)})

        await asyncio.sleep(SETTLE)

        ratios, halo = [], []
        for n in range(frames):
            img = await shoot(client, name)
            img.save(out / "frames" / f"{pack_id}-{pid}-{slug}-{n}.png")
            m = metrics(img)
            ratios.append(abs(m["luma"] - baseline) if unit == "diff" else m["core"])
            halo.append(m["lit"])
            if n == frames // 2:
                over_checker(img).save(out / "sheet" / f"{pack_id}-{pid}-{slug}.png")
            if n < frames - 1:
                await asyncio.sleep(FRAME_GAP)

        report["cells"].append({
            "pack": pack_id, "id": pid, "kind": kind, "state": state, "unit": unit,
            "min": round(min(ratios), 4), "max": round(max(ratios), 4),
            "spread": round(max(ratios) - min(ratios), 4),
            # "frozen" is a claim about MOVEMENT, and one sample cannot support it. With
            # --frames 1 every cell was reporting "frozen (no change)" when nothing had been
            # compared to anything.
            "movement_known": frames > 1,
            "halo_max": round(max(halo), 4),
            "empty": max(ratios) == 0.0,
        })

    if motion:
        await _capture_motion(client, pack_id, pid, kind, name, out)

    await client.request("RemoveInput", {"inputName": name})


async def _capture_motion(client, pack_id, pid, kind, name, out) -> None:
    """A short GIF under the beat state. A still cannot show whether the movement reads, and the
    movement is the product."""
    await _set_audio(client, name, kind,
                     {"audio_mode": 1, "audio_source": tap_name(MOTION_STATE)})
    await asyncio.sleep(SETTLE)

    seq = []
    for _ in range(MOTION_FRAMES):
        frame = over_checker(await shoot(client, name))
        seq.append(frame.convert("P", palette=Image.Palette.ADAPTIVE))
        await asyncio.sleep(MOTION_GAP)
    seq[0].save(out / "motion" / f"{pack_id}-{pid}.gif", save_all=True,
                append_images=seq[1:], duration=int(MOTION_GAP * 1000), loop=0)


# --------------------------------------------------------------------------- contact sheet

def build_sheet(report: dict, out: Path, pack_id: str) -> Path | None:
    cells = report["cells"]
    if not cells:
        return None

    presets = []
    for c in cells:
        if c["id"] not in presets:
            presets.append(c["id"])
    states = [s for s, _b, _n in STATES]

    tw, th = W // 2, H // 2
    pad, gutter, header, caption = 10, 210, 96, 30
    sheet_w = gutter + len(states) * (tw + pad) + pad
    sheet_h = header + len(presets) * (th + caption + pad) + pad
    sheet = Image.new("RGBA", (sheet_w, sheet_h), (24, 24, 28, 255))
    d = ImageDraw.Draw(sheet)
    f_title, f_head, f_small = _font(19), _font(15), _font(12)

    d.text((pad, 12), f"Foxfire look-dev — pack '{pack_id}'", font=f_title, fill=(240, 238, 232))
    d.text((pad, 38),
           "checkerboard = transparent · core = fraction of solid pixels (alpha>200) · "
           "diff = how much the filter changed its source · Δ = movement between frames",
           font=f_small, fill=(150, 150, 158))

    for ci, (state, _b, note) in enumerate(STATES):
        x = gutter + ci * (tw + pad)
        d.text((x, header - 36), _fit(d, state, f_head, tw), font=f_head, fill=(232, 228, 220))
        d.text((x, header - 18), _fit(d, note, f_small, tw), font=f_small,
               fill=(140, 140, 150))

    by_cell = {(c["id"], c["state"]): c for c in cells}
    for ri, pid in enumerate(presets):
        y = header + ri * (th + caption + pad)
        kind = next(c["kind"] for c in cells if c["id"] == pid)
        avail = gutter - pad * 2
        d.text((pad, y + 4), _fit(d, pid, f_head, avail), font=f_head,
               fill=(255, 190, 140))
        d.text((pad, y + 24), _fit(d, kind, f_small, avail), font=f_small,
               fill=(150, 150, 158))
        if (out / "motion" / f"{pack_id}-{pid}.gif").exists():
            d.text((pad, y + 42), _fit(d, f"motion/{pack_id}-{pid}.gif", f_small, avail),
                   font=f_small, fill=(120, 150, 170))

        for ci, state in enumerate(states):
            x = gutter + ci * (tw + pad)
            slug = state.replace(" ", "-")
            tile = out / "sheet" / f"{pack_id}-{pid}-{slug}.png"
            if tile.exists():
                sheet.alpha_composite(Image.open(tile).convert("RGBA").resize((tw, th)), (x, y))
            else:
                d.rectangle([x, y, x + tw - 1, y + th - 1], fill=(40, 30, 30, 255))
                d.text((x + 8, y + th // 2), "not captured", font=f_small, fill=(220, 120, 120))

            c = by_cell.get((pid, state))
            if c:
                u = c.get("unit", "core")
                if c["empty"]:
                    label = ("EMPTY — filter changed nothing" if u == "diff"
                             else "EMPTY — nothing rendered")
                    colour = (235, 120, 120)
                elif not c.get("movement_known", True):
                    label = f"{u} {c['max']:.3f} · single frame"
                    colour = (150, 170, 200)
                elif c["spread"] == 0.0:
                    label = f"{u} {c['max']:.3f} · frozen (no change)"
                    colour = (225, 195, 120)
                else:
                    label = f"{u} {c['min']:.3f}–{c['max']:.3f} · Δ{c['spread']:.3f}"
                    colour = (150, 200, 160)
                d.text((x, y + th + 6), _fit(d, label, f_small, tw), font=f_small, fill=colour)

    path = out / "contact-sheet.png"
    sheet.convert("RGB").save(path)
    return path


# --------------------------------------------------------------------------- orchestration

async def drive(pack_dir: Path, out: Path, frames: int, motion: bool) -> dict:
    report: dict = {"obs": None, "pack": None, "presets_inspected": 0, "states": len(STATES),
                    "cells": [], "skipped": [], "warnings": []}
    ws = None
    try:
        ws, client = await ff_proof.open_client("look-dev")
        report["obs"] = (await client.request("GetVersion")).get("obsVersion")

        await client.request("CreateScene", {"sceneName": "fflook"})
        await client.request("SetCurrentProgramScene", {"sceneName": "fflook"})

        tmp = Path(tempfile.mkdtemp(prefix="ff-look-audio-"))
        await create_taps(client, "fflook", tmp)

        pack_id, presets = proof._load_manifest(pack_dir)
        report["pack"] = pack_id
        for preset in presets:
            await capture_preset(client, pack_id, preset, out, frames, motion, report)
            report["presets_inspected"] += 1
    except Exception as e:
        report["warnings"].append(f"capture aborted: {e!r}")
    finally:
        if ws is not None:
            await ws.close()
    return report


async def run(args) -> int:
    repo = Path(args.plugin_build).resolve()
    pack_dir = Path(args.pack).resolve()
    out = Path(args.out).resolve()
    for sub in ("frames", "sheet", "motion"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    cfg_root = Path(tempfile.mkdtemp(prefix="ff-look-"))
    obs_cfg = cfg_root / "obs-studio"
    obs_cfg.mkdir(parents=True)
    proof.write_ws_config(obs_cfg)
    proof.install_plugin(repo, obs_cfg)
    proof.install_pack(pack_dir, obs_cfg)

    env = dict(os.environ, XDG_CONFIG_HOME=str(cfg_root))
    procs = subprocess.Popen(
        ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi", "--minimize-to-tray"],
        # --multi: without it a second OBS opens an "already running" warning dialog;
        # under Xvfb nobody can click it, so the process hangs to the boot timeout and
        # never writes a log -- indistinguishable from a crash.
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)

    report: dict = {"cells": [], "skipped": [], "warnings": [], "presets_inspected": 0}
    try:
        proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
        report = await drive(pack_dir, out, args.frames, not args.no_motion)
    except TimeoutError as e:
        report["warnings"].append(str(e))
    finally:
        proof.terminate_process_group(procs)
        logs = sorted(obs_cfg.glob("logs/*.txt"))
        if logs:
            for line in logs[-1].read_text(errors="replace").splitlines():
                if proof.WARN_TAG in line and any(t in line for t in proof.ALLOWED_WARNING_TEXTS):
                    continue
                if proof.WARN_TAG in line or proof.ERROR_TAG in line:
                    report["warnings"].append(line.strip())
            shutil.copy(logs[-1], out / "obs.log")
        if args.keep:
            print(f"look: sandbox kept at {cfg_root}")
        else:
            shutil.rmtree(cfg_root, ignore_errors=True)

    sheet = build_sheet(report, out, report.get("pack") or pack_dir.name)
    (out / "report.json").write_text(json.dumps(report, indent=2))

    empty = [c for c in report["cells"] if c["empty"]]
    frozen = [c for c in report["cells"]
              if not c["empty"] and c.get("movement_known", True) and c["spread"] == 0.0]
    print(f"look: {report['presets_inspected']} preset(s) x {len(STATES)} audio state(s) "
          f"= {len(report['cells'])} frame group(s); {len(empty)} empty, {len(frozen)} frozen, "
          f"{len(report['skipped'])} skipped, {len(report['warnings'])} warning(s)")
    for c in report["cells"]:
        flag = ("EMPTY" if c["empty"]
                else "" if not c.get("movement_known", True)
                else "frozen" if c["spread"] == 0.0 else "")
        print(f"  {c['id']:<14} {c['state']:<12} {c['unit']:<4} {c['min']:.4f}..{c['max']:.4f} "
              f"(halo {c['halo_max']:.4f}) {flag}")
    for s in report["skipped"]:
        print(f"  SKIPPED {s['id']}: {s['why']}")
    for w in report["warnings"]:
        print("  WARN", w)

    if sheet:
        print(f"\nlook: contact sheet -> {sheet}")
        print(f"      view it with:  cshow {sheet}")
    else:
        print("\nlook: NOTHING RENDERED -- no contact sheet written")
        return 2
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True, help="built engine repo (has install-local.sh)")
    ap.add_argument("--pack", required=True, help="pack dir (pack.json + effects/) to look at")
    ap.add_argument("--out", default="build_x86_64/look", help="where frames and the sheet go")
    ap.add_argument("--frames", type=int, default=3,
                    help="stills per preset per state; >1 is what makes a frozen preset visible")
    ap.add_argument("--no-motion", action="store_true", help="skip the per-preset GIF")
    ap.add_argument("--keep", action="store_true", help="keep the temp OBS sandbox")
    return asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
