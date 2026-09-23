#!/usr/bin/env python3
"""packforge-facing render proof CLI.

Entry point: `python3 tools/proof.py --plugin-build <engine repo> [--pack <pack dir>] [--out <out
dir>] [--keep]`. packforge (a separate tool) runs this and then reads `<out>/report.json`.

This does NOT reimplement the render proof. It boots a throwaway OBS under Xvfb (the same sandbox
recipe tools/render-proof.sh uses), installs the built plugin, then runs tools/ff_proof.py's full
31-check harness against it unchanged -- silence/tone/gap/restore-defaults/pack-install/the
properties-vs-render stress race/destroy, the Foxfire Effects filter proof, the spatial (orientation
+ blur-extent) proof, and the transparency (alpha-convention) proof. That harness's check()/CHECKS/
FAILURES bookkeeping is reused, not copied, for everything below.

When --pack is given, this ALSO copies that pack into the sandbox and, for every preset its
pack.json declares, creates the right OBS object and registers one named ff_proof.check() per
preset so a blank preset fails the run:

* kind visualizer/overlay: a foxfire_visualizer input, driven from the harness's still-live 'tone'
  audio source, one screenshot, analysed directly (analyse()).
* kind effects: a color_source_v3 base with a foxfire_effects filter. This is measured as a BEFORE
  (no filter) / AFTER (filter attached) pair -- analyse_diff() -- not a single shot. A single shot
  of the opaque grey base alone would read as "non-blank" regardless of what the filter did (or
  didn't do): every pixel is already opaque and non-black before any filter runs, so a filter that
  fails to compile, loads no layers, is a no-op, or is licence-refused would pass right alongside a
  working one. Diffing against a baseline taken before the filter existed measures what the filter
  itself contributed. Reuses the same before/after idea run_filter() in ff_proof.py already uses
  for its own filter proof, not a third copy of that comparison.
* any other kind is a named, failing check -- never a silent pass-through.

Any --pack run also generates a throwaway copy of the given pack, with its id rewritten and placed
outside data/packs/, and installs THAT via install_pack() as well (see make_install_probe()). This
exists because install-local.sh copies the repo's own data/ (including data/packs/demo) into the
sandbox's plugin data dir, and the engine scans that bundled root before the user config dir and
returns the first id match -- so `--pack data/packs/demo` is silently served from the bundled copy
in every run, and install_pack()'s copy into the user config dir is never actually exercised. The
probe pack's id can only have been found via install_pack(); report.json names it in
"install_probe_pack" and its presets carry that id.

Without --pack only the unchanged 31-check harness runs, matching tools/render-proof.sh's
behaviour exactly.

report.json's "checks_armed" is the total number of named checks armed (harness + any per-preset
checks) -- the most honest answer to "how much did this gate actually look at" for a proof whose
only unit of work is a named check(). "presets_inspected" counts preset rows separately.
"checks_armed" is 0, and the process exits 2, only when the harness could not even connect to OBS
(a boot failure), not merely when --pack was omitted. A pack.json with an empty or absent
"presets" list, or one outside the engine's own 1..64 range (src/ff-pack.c:196), is a named
failing check, not a silent no-op -- an empty pack proves nothing about the pack it was pointed at,
so it must not exit 0.

The obs.log scanner below keys on the severity TAG plugin-support.c.in now stamps into every log
line ("[obs-foxfire] warn: " / "[obs-foxfire] error: "), not on guessing severity from message
wording -- OBS's log has no severity marker of its own, so a keyword search over the message text
(the previous approach) misses most real warnings, whose wordings were never written to contain
"warning" or "failed". See ALLOWED_WARNING_TEXTS for the short, named list of expected exceptions.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import io
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ff_proof  # noqa: E402  -- the 31-check harness this wraps; see its module docstring

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

PORT = ff_proof.PORT  # single source of truth -- see ff_proof.py; was duplicated here (Minor 11)
BOOT_TIMEOUT = 60.0  # seconds to wait for obs-websocket's port to open
SCREEN = "1920x1080x24"

# src/ff-pack.h FF_MAX_PRESETS, mirrored here so the gate refuses the same 1..64 range the engine
# itself refuses at src/ff-pack.c:196. Kept in sync by hand, same as ff_proof.EXPECTED_CHECKS.
FF_MAX_PRESETS = 64
VISUALIZER_KINDS = {"visualizer", "overlay"}
EFFECTS_KINDS = {"effects"}
ALERT_KINDS = {"alert"}
# Rendered elsewhere: a transition exists only during a cut and cannot be screenshotted at all
# (it is not in the canvas by name). tools/transition-proof.py drives a real one.
TRANSITION_KINDS = {"transition"}


def _kinds_the_loader_accepts(repo: Path) -> set[str]:
    """The accepted kinds, read out of src/ff-pack.c rather than written here a second time.

    This list has now drifted twice in one day. Adding `transition` to the engine and not to the
    loader refused the whole bundled pack; adding it to the loader and not to THIS file failed
    every run of the render proof. A third hand-maintained copy would drift a third time, so the
    C source is the single source of truth and the assertion below is what notices.
    """
    src = (repo / "src" / "ff-pack.c").read_text()
    m = re.search(r"static const char \*KINDS\[\] = \{([^}]*)\}", src)
    if not m:
        raise RuntimeError("could not find KINDS[] in src/ff-pack.c -- if it moved or was renamed, "
                           "this check has to follow it rather than be deleted")
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def check_kind_lists_agree(repo: Path) -> None:
    mine = VISUALIZER_KINDS | EFFECTS_KINDS | ALERT_KINDS | TRANSITION_KINDS
    theirs = _kinds_the_loader_accepts(repo)
    ff_proof.check("this proof knows every preset kind the loader accepts",
                   mine == theirs,
                   f"proof.py has {sorted(mine)}, src/ff-pack.c accepts {sorted(theirs)}"
                   + (f"; MISSING HERE: {sorted(theirs - mine)}" if theirs - mine else "")
                   + (f"; NOT ACCEPTED THERE: {sorted(mine - theirs)}" if mine - theirs else ""))

WARN_TAG = "[obs-foxfire] warn: "
ERROR_TAG = "[obs-foxfire] error: "
# Every entry here is a warn:/error: line the harness EXPECTS to produce on every normal run --
# proof that a defence fired correctly, not a defect. Any warn:/error: line that matches none of
# these fails the run. Delete an entry the day its cause goes away (the pubkey one the day
# packforge bakes in the real key; the other two are permanent as long as ff_proof.py's symlink-zip
# install attempt stays part of the standard harness -- see tools/ff_proof.py's "Critical-1" check).
ALLOWED_WARNING_TEXTS = (
    # src/ff-pack.c ff_packs_scan(): fires once per process while FF_PUBLIC_KEY is all zeros
    # (see pubkey_is_zero()) -- expected until packforge bakes the real key in.
    "licence: public key not set; paid packs will not verify",
    # src/ff-pack.c ff_packs_install_zip(): the symlink-zip attack ff_proof.py deliberately drives
    # at "ff" (tools/build_symlink_pack_zip.py) must be refused, and IS refused, which logs a
    # warn: line by the same obs_log(LOG_WARNING, ...) every other install refusal uses. A missing
    # line here would mean the entry no longer fires, not that the log got quieter.
    "pack install: zip contains a symlink entry, refused:",
    "install '/tmp/ff-symlink-attack.zip': refused: zip must not contain symlinks",
    # the second attack zip: same defence, driven through the CLEANUP path (its manifest declares
    # an impossible min_engine so extraction is attempted and then torn down). Listed separately
    # rather than loosening the two entries above to a shared substring: the full path is a
    # deliberate fail-loud coupling, so renaming either fixture breaks the run instead of quietly
    # matching something else.
    "install '/tmp/ff-symlink-cleanup-test.zip': refused: zip must not contain symlinks",
)


def write_ws_config(obs_cfg: Path) -> None:
    d = obs_cfg / "plugin_config" / "obs-websocket"
    d.mkdir(parents=True, exist_ok=True)
    (d / "config.json").write_text(json.dumps({
        "first_load": False, "server_enabled": True, "server_port": PORT,
        "alerts_enabled": False, "auth_required": False, "server_password": "",
    }))
    # skips the first-run wizard, which otherwise owns the UI thread for the whole session --
    # same trick tools/render-proof.sh uses
    (obs_cfg / "user.ini").write_text("[General]\nFirstRun=true\nConfirmOnExit=false\n")


def install_plugin(repo: Path, obs_cfg: Path) -> None:
    subprocess.run([str(repo / "install-local.sh"), str(obs_cfg)], check=True)


# Every Foxfire plugin reads ONE shared packs directory -- not its own per-module one -- so that
# a pack installed through any of them is visible to all of them. See ff_shared_config_path in
# src/ff-pack.c. This constant and that function are the two halves of the same decision: change
# one and the harness installs where the plugin does not look, which reads as "the pack does not
# render" rather than as "the pack is not there".
PACKS_DIR = ("plugin_config", "foxfire", "packs")


def install_pack(pack_dir: Path, obs_cfg: Path) -> None:
    dst = obs_cfg.joinpath(*PACKS_DIR) / pack_dir.name
    if dst.exists():
        shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(pack_dir, dst)


def make_install_probe(pack_dir: Path, tmp_root: Path) -> Path:
    """Copies pack_dir into a scratch directory OUTSIDE data/packs/, with its id rewritten, so a
    later install_pack() + enumerate of it can only have come from install_pack()'s copy into the
    sandbox's user config dir -- never from install-local.sh's bundled copy of the repo's own
    data/, which shadows any --pack pointed at data/packs/<id> (ff_packs_scan scans the bundled
    root first and ff_packs_find returns the first id match; src/ff-pack.c:286-296, 313-319).
    Generated fresh in the sandbox each run, per the controller's ruling on Important 3, rather
    than committing a second fixture pack."""
    probe_dir = tmp_root / "install-probe"
    if probe_dir.exists():
        shutil.rmtree(probe_dir)
    shutil.copytree(pack_dir, probe_dir)
    manifest_path = probe_dir / "pack.json"
    manifest = json.loads(manifest_path.read_text())
    manifest["id"] = f"{manifest['id']}-installprobe"
    manifest_path.write_text(json.dumps(manifest))
    return probe_dir


def wait_for_port(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s = socket.socket()
        try:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        finally:
            s.close()
        time.sleep(0.5)
    raise TimeoutError(f"obs-websocket never opened port {port} within {timeout:.0f}s")


def wait_for_port_free(port: int, timeout: float = 30.0) -> None:
    """Blocks until nothing is listening on `port`. Call BEFORE launching OBS.

    Every proof in this directory uses the same fixed port, so running two in sequence races: the
    previous OBS can still be shutting down with its socket open, wait_for_port() sees an open
    port immediately, and the new harness connects to the DYING instance. That fails as a request
    timing out several steps later -- "CreateScene returns", say -- which reads as a defect in
    whatever the proof was testing rather than as the wrong OBS answering. Worse, it could just as
    easily have succeeded against the old process and reported on a plugin build that was not the
    one under test.

    Waiting is better than picking a random port per run: a fixed port is what the sandbox config
    file written next to it declares, and two harnesses genuinely should not run at once."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s = socket.socket()
        try:
            if s.connect_ex(("127.0.0.1", port)) != 0:
                return
        finally:
            s.close()
        time.sleep(0.5)
    raise TimeoutError(
        f"port {port} is still in use after {timeout:.0f}s -- another OBS is running on it, and "
        f"connecting would test THAT one instead of the build under test")


def analyse(png_bytes: bytes) -> tuple[float, float]:
    """(nonblank_ratio, dominant_hue): same 'lit' definition as ff_proof.Shot (alpha>8), plus a
    max(rgb)>8 floor so a fully-opaque-but-black frame does not count as lit. hue is the mean lit
    colour's HSV hue in degrees, or -1 when nothing is lit."""
    im = np.asarray(Image.open(io.BytesIO(png_bytes)).convert("RGBA"))
    px = im.reshape(-1, 4)
    n = len(px)
    mask = (px[:, 3] > 8) & (px[:, :3].max(axis=1) > 8)
    lit = px[mask]
    ratio = (len(lit) / n) if n else 0.0
    if len(lit) == 0:
        return ratio, -1.0
    r, g, b = (lit[:, :3].mean(axis=0)).astype(int)
    hue = Image.new("RGB", (1, 1), (int(r), int(g), int(b))).convert("HSV").getpixel((0, 0))[0] * 360 / 255
    return ratio, hue


def analyse_diff(before_bytes: bytes, after_bytes: bytes) -> tuple[float, float]:
    """(nonblank_ratio, dominant_hue) from what CHANGED between two shots of the same base, taken
    before and after a foxfire_effects filter is attached to it -- not from either shot alone.

    Important 1: analyse() reads whatever the backdrop already looks like. The effects base is an
    opaque, non-black color_source_v3 (0xFF404040), so analyse() alone pins ratio at 1.0 before the
    filter does anything -- a filter that fails to compile, loads no layers, is a no-op, or is
    licence-refused would all read identically to a working one. Diffing against a baseline taken
    before the filter existed measures the filter's own contribution instead.

    ratio: fraction of pixels whose RGB moved by more than 8/255 in any channel (same threshold as
    analyse()'s alpha>8 "lit" floor). hue: the mean HSV hue of the POSITIVE part of the change
    (what the filter added), over the changed pixels only, or -1 when nothing changed.

    If a future preset reads as noise here (e.g. a filter that shifts hue without changing
    coverage), the fix is to gate on mean-channel delta instead of this coverage ratio -- not to
    weaken the >0.01 threshold below."""
    before = np.asarray(Image.open(io.BytesIO(before_bytes)).convert("RGBA")).reshape(-1, 4)[:, :3].astype(np.int16)
    after = np.asarray(Image.open(io.BytesIO(after_bytes)).convert("RGBA")).reshape(-1, 4)[:, :3].astype(np.int16)
    n = len(after)
    delta = after - before
    changed = np.abs(delta).max(axis=1) > 8
    ratio = float(changed.sum() / n) if n else 0.0
    if not changed.any():
        return ratio, -1.0
    contrib = np.clip(delta[changed], 0, 255).mean(axis=0).astype(int)
    r, g, b = contrib
    hue = Image.new("RGB", (1, 1), (int(r), int(g), int(b))).convert("HSV").getpixel((0, 0))[0] * 360 / 255
    return ratio, float(hue)


async def _shoot(client, source_name: str) -> bytes:
    shot = await client.request("GetSourceScreenshot", {
        "sourceName": source_name, "imageFormat": "png", "imageWidth": 640, "imageHeight": 360})
    return base64.b64decode(shot["imageData"].split(",", 1)[1])


def write_filter_base(out: Path) -> Path:
    """The picture a FILTER preset is proved against.

    Everything in it is there because some filter needs it and a flat field does not have it:

      * hard edges and a fine checker -- a chromatic split has nothing to separate without them,
        and a blur has nothing to soften
      * a smooth ramp -- a colour grade remaps luminance, so it needs the whole range present, and
        banding in the result is visible against a ramp and invisible against a patch
      * one small blown highlight -- a bloom that only lights pixels above a threshold needs at
        least one, and a threshold of 0.72 is typical, so this goes to full white
      * mid-grey skin-ish patches -- the midtones a grade is supposed to protect

    Written fresh into the proof's own out/ each run rather than committed: it is a fixture, and a
    fixture nobody can regenerate is one nobody dares change.
    """
    from PIL import Image, ImageDraw
    w, h = 640, 360
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for x in range(w):                                    # horizontal ramp, full 0..255
        d.line([(x, 0), (x, h)], fill=(x * 255 // (w - 1),) * 3)
    # The checker runs the FULL width, as a modulation of the ramp rather than flat dark squares.
    # Confined to the left half it sat entirely in the black end, where an edge has almost no
    # contrast to displace -- a chromatic split reads as nothing at all, which is exactly how the
    # first version of this pattern hid three presets that were working.
    src = im.load()
    for cy in range(0, h, 16):
        for cx in range(0, w, 16):
            if ((cx // 16) + (cy // 16)) % 2 == 0:
                for y in range(cy, min(cy + 16, h)):
                    for x in range(cx, min(cx + 16, w)):
                        r, g, b = src[x, y]
                        src[x, y] = (int(r * 0.55), int(g * 0.55), int(b * 0.6))
    d.ellipse([470, 60, 540, 130], fill=(255, 255, 255))  # the highlight a bloom needs
    d.rectangle([420, 210, 520, 300], fill=(196, 150, 122))   # midtone, warm
    d.rectangle([530, 210, 620, 300], fill=(122, 150, 196))   # midtone, cool
    p = out / "filter-base.png"
    im.save(p)
    return p


async def enumerate_pack(client, pack_id: str, preset_decls: list[dict], out: Path, report: dict) -> None:
    """Drives every preset pack.json declares through the sandbox's live OBS, reusing the harness's
    'tone' audio source (still present -- ff_proof.run() creates it and never removes it) and its
    'ffproof' scene (still the current program scene) so audio-reactive presets actually light up.
    Registers one ff_proof.check() per preset, and per-preset ROW is appended to report["presets"]
    as it is produced -- not returned and assigned after the loop -- so a preset that raises
    partway through the pack still leaves the rows already proved (Minor 5), and any screenshot
    already taken has a matching row instead of sitting orphaned.

    kind visualizer/overlay: one screenshot, analysed directly (analyse()).
    kind effects: two screenshots of the same color_source_v3 base -- before the foxfire_effects
    filter is attached, and after -- analysed as a diff (analyse_diff()); see Important 1.
    Any other kind is a named, failing check (Minor 6) -- never treated as a visualizer."""
    base_png = write_filter_base(out)
    for pr in preset_decls:
        name = f"proof-{pack_id}-{pr['id']}"
        kind = pr["kind"]
        settings = {"pack": pack_id, "preset": pr["id"], "audio_mode": 1, "audio_source": "tone",
                    "width": 640, "height": 360}

        if kind in EFFECTS_KINDS:
            # A PATTERN, not a flat colour. The base used to be a solid 0xFF404040 field, and a
            # flat field is exactly what a SPATIAL filter cannot change: blurring one grey gives
            # the same grey, and shifting its red and blue channels sideways gives the same grey
            # again. Measured -- six new bloom and chroma presets came back "diff 0.000 BLANK"
            # while rendering perfectly, because the fixture could not express what they do. The
            # pattern carries edges, a gradient, and a highlight above any sane bloom threshold,
            # so every filter this pack ships has something to act on.
            await client.request("CreateInput", {
                "sceneName": "ffproof", "inputName": name, "inputKind": "image_source",
                "inputSettings": {"file": str(base_png)}})
            await asyncio.sleep(1.0)
            before_raw = await _shoot(client, name)
            await client.request("CreateSourceFilter", {
                "sourceName": name, "filterName": "fx", "filterKind": "foxfire_effects",
                "filterSettings": settings})
            await asyncio.sleep(1.5)
            raw = await _shoot(client, name)
            (out / f"{pack_id}-{pr['id']}.png").write_bytes(raw)
            ratio, hue = analyse_diff(before_raw, raw)
            unit = "diff"
        elif kind in ALERT_KINDS:
            # An alert draws only while an alert is RUNNING, so it has to be fired before it can
            # be screenshotted -- a shot of an idle alert source is correctly empty, and treating
            # that as a blank preset would fail every alert pack ever written. Fired through the
            # properties button, which is the same path a streamer uses.
            #
            # The shot is taken partway in rather than immediately: an alert pack that animates
            # its entrance (the `progress` builtin) is at its smallest on frame one, and judging
            # it there would mark a working pack blank.
            await client.request("CreateInput", {
                "sceneName": "ffproof", "inputName": name, "inputKind": "foxfire_alert",
                "inputSettings": {"pack": pack_id, "preset": pr["id"], "width": 640, "height": 360,
                                  "duration": 6.0, "template": "{name} followed!"}})
            await asyncio.sleep(1.0)
            await client.request("PressInputPropertiesButton",
                                 {"inputName": name, "propertyName": "test"})
            await asyncio.sleep(2.0)
            raw = await _shoot(client, name)
            (out / f"{pack_id}-{pr['id']}.png").write_bytes(raw)
            ratio, hue = analyse(raw)
            unit = "nonblank"
        elif kind in VISUALIZER_KINDS:
            await client.request("CreateInput", {
                "sceneName": "ffproof", "inputName": name, "inputKind": "foxfire_visualizer",
                "inputSettings": settings})
            await asyncio.sleep(1.5)
            raw = await _shoot(client, name)
            (out / f"{pack_id}-{pr['id']}.png").write_bytes(raw)
            ratio, hue = analyse(raw)
            unit = "nonblank"
        elif kind in TRANSITION_KINDS:
            # Skipped here, and SAID rather than passed over: a transition renders only during a
            # cut between two scenes, so there is no screenshot of one to take. Covered by
            # tools/transition-proof.py, which drives a real cut and reads the recording.
            print(f"  [skip] {pack_id}/{pr['id']}: kind 'transition' -- covered by transition-proof.py")
            report["presets"].append({"pack": pack_id, "id": pr["id"], "kind": kind,
                                      "nonblank_ratio": None, "dominant_hue": None,
                                      "ok": None, "skipped": "rendered by transition-proof.py"})
            continue
        else:
            ff_proof.check(f"pack preset {pack_id}/{pr['id']} has a recognised kind", False,
                            f"kind={kind!r} not in "
                            f"{sorted(VISUALIZER_KINDS | EFFECTS_KINDS | ALERT_KINDS | TRANSITION_KINDS)}")
            report["presets"].append({"pack": pack_id, "id": pr["id"], "kind": kind,
                                       "nonblank_ratio": 0.0, "dominant_hue": -1.0, "ok": False})
            continue

        ok = ratio > 0.01
        ff_proof.check(f"pack preset {pack_id}/{pr['id']} renders non-blank ({unit})", ok,
                        f"{unit}_ratio={ratio:.4f}, threshold >0.01, dominant_hue={hue:.1f}")
        report["presets"].append({"pack": pack_id, "id": pr["id"], "kind": kind,
                                   "nonblank_ratio": round(ratio, 4), "dominant_hue": round(hue, 1),
                                   "ok": ok})
        await client.request("RemoveInput", {"inputName": name})


def _load_manifest(pack_dir: Path) -> tuple[str, list[dict]]:
    manifest = json.loads((pack_dir / "pack.json").read_text())
    return manifest["id"], manifest.get("presets", [])


async def drive(pack_dir: Path | None, probe_dir: Path | None, out: Path, port: int) -> dict:
    report: dict = {"obs": None, "checks_armed": 0, "presets_inspected": 0, "presets": [],
                     "install_probe_pack": None, "warnings": [], "checks": {}}
    expected = ff_proof.EXPECTED_CHECKS

    try:
        await ff_proof.main(port)
    except Exception as e:  # a boot/connect failure that guard() couldn't turn into a named FAIL
        report["warnings"].append(f"harness aborted before completing: {e!r}")

    ws = None
    try:
        ws, client = await ff_proof.open_client("proof enumeration")
        ver = await client.request("GetVersion")
        report["obs"] = ver.get("obsVersion")

        if pack_dir is not None:
            pack_id, preset_decls = _load_manifest(pack_dir)
            n = len(preset_decls)
            # Important 2: read the count ONCE, up front, independent of what actually gets armed
            # below -- expected must not be derived from the same list that produces the armed
            # count, or the armed-vs-expected comparison can never fire. An empty or absent
            # "presets" list, or one outside the engine's own refusal range, is a NAMED failure,
            # never a silent no-op that exits 0 having proven nothing about the pack.
            if not (1 <= n <= FF_MAX_PRESETS):
                ff_proof.check(f"pack {pack_id} declares a valid preset count", False,
                                f"{n} preset(s) in pack.json, expected 1..{FF_MAX_PRESETS} "
                                f"(matches the engine's own refusal, src/ff-pack.c:196)")
            else:
                # Only presets this proof can actually render arm a check. A transition arms one
                # in transition-proof.py instead, and counting it here would make expected
                # permanently exceed armed -- which is the signal that something did not run.
                expected += sum(1 for d in preset_decls if d.get("kind") not in TRANSITION_KINDS)
                try:
                    await enumerate_pack(client, pack_id, preset_decls, out, report)
                except Exception as e:
                    report["warnings"].append(f"pack enumeration aborted: {e!r}")

        if probe_dir is not None:
            # Important 3: prove install_pack() actually got used, with a pack id that cannot have
            # come from install-local.sh's bundled copy of data/packs/ -- see make_install_probe().
            # +1 below is the "found and enumerated" check() itself, so a fully successful run's
            # armed count still equals expected exactly (Important 2's armed-vs-expected gate).
            probe_id, probe_decls = _load_manifest(probe_dir)
            expected += sum(1 for d in probe_decls if d.get("kind") not in TRANSITION_KINDS) + 1
            try:
                await enumerate_pack(client, probe_id, probe_decls, out, report)
            except Exception as e:
                report["warnings"].append(f"install-probe enumeration aborted: {e!r}")
            report["install_probe_pack"] = probe_id
            named = [p for p in report["presets"] if p["pack"] == probe_id]
            ff_proof.check(
                f"install_pack() pack '{probe_id}' (outside data/packs/, id != a bundled pack's) "
                f"was found and enumerated", len(named) == len(probe_decls),
                f"{len(named)}/{len(probe_decls)} preset row(s) recorded for {probe_id}")
    except Exception as e:
        report["warnings"].append(f"pack enumeration aborted: {e!r}")
    finally:
        if ws is not None:
            try:
                await asyncio.wait_for(ws.close(), timeout=5)
            except asyncio.TimeoutError:
                pass

    report["checks_armed"] = len(ff_proof.CHECKS)
    report["presets_inspected"] = len(report["presets"])
    passed = len(ff_proof.CHECKS) - len(ff_proof.FAILURES)
    report["checks"] = {"passed": passed, "armed": len(ff_proof.CHECKS), "expected": expected}
    return report


def terminate_process_group(proc: subprocess.Popen) -> None:
    """Mirrors tools/render-proof.sh's teardown (pkill children, then the process itself, SIGKILL
    if it's still alive after a grace period) but via a real process group, since xvfb-run's child
    (the actual obs process) would otherwise survive a plain proc.terminate()."""
    try:
        pgid = os.getpgid(proc.pid)
    except ProcessLookupError:
        return
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return
    for _ in range(20):
        if proc.poll() is not None:
            break
        time.sleep(1)
    if proc.poll() is None:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass


async def run(args) -> int:
    repo = Path(args.plugin_build).resolve()
    # Before OBS is even started: this costs nothing and its failure explains every
    # "unrecognised kind" below it, which otherwise read as broken packs.
    check_kind_lists_agree(repo)
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    pack_dir = Path(args.pack).resolve() if args.pack else None

    cfg_root = Path(tempfile.mkdtemp(prefix="ff-proof-"))
    obs_cfg = cfg_root / "obs-studio"
    obs_cfg.mkdir(parents=True)
    write_ws_config(obs_cfg)
    install_plugin(repo, obs_cfg)
    probe_dir = None
    if pack_dir is not None:
        install_pack(pack_dir, obs_cfg)
        probe_dir = make_install_probe(pack_dir, cfg_root)
        install_pack(probe_dir, obs_cfg)

    env = dict(os.environ, XDG_CONFIG_HOME=str(cfg_root))
    wait_for_port_free(PORT)  # never connect to a previous run's dying OBS
    proc = subprocess.Popen(
        ["xvfb-run", "-a", "-s", f"-screen 0 {SCREEN}", "obs", "--multi", "--minimize-to-tray"],
        # --multi: without it a second OBS opens a "already running" warning dialog;
        # under Xvfb nobody can click it, so the process hangs to the boot timeout and
        # never writes a log -- indistinguishable from a crash.
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)

    report = {"obs": None, "checks_armed": 0, "presets_inspected": 0, "presets": [],
              "install_probe_pack": None, "warnings": [], "checks": {
                  "passed": 0, "armed": 0, "expected": ff_proof.EXPECTED_CHECKS}}
    try:
        wait_for_port(PORT, BOOT_TIMEOUT)
        report = await drive(pack_dir, probe_dir, out, PORT)
    except TimeoutError as e:
        report["warnings"].append(str(e))
    finally:
        terminate_process_group(proc)
        logs = sorted(obs_cfg.glob("logs/*.txt"))
        if logs:
            latest = logs[-1]
            log_text = latest.read_text(errors="replace")

            # Track which expected warnings actually appeared in the log
            found_expected = {text: False for text in ALLOWED_WARNING_TEXTS if text != "licence: public key not set; paid packs will not verify"}

            for line in log_text.splitlines():
                if WARN_TAG in line:
                    # Check if this is an expected warning
                    if any(t in line for t in ALLOWED_WARNING_TEXTS):
                        # Track which expected warnings we found (except pubkey which is conditional)
                        for text in found_expected:
                            if text in line:
                                found_expected[text] = True
                        continue  # expected -- see ALLOWED_WARNING_TEXTS
                    # Unexpected warning
                    report["warnings"].append(line.strip())
                elif ERROR_TAG in line:
                    report["warnings"].append(line.strip())

            # Check that all non-conditional expected warnings actually fired
            for text, found in found_expected.items():
                if not found:
                    report["warnings"].append(f"expected warning did not fire: {text!r}")

            shutil.copy(latest, out / "obs.log")
        if args.keep:
            print(f"proof: sandbox kept at {cfg_root}")
        else:
            shutil.rmtree(cfg_root, ignore_errors=True)

    (out / "report.json").write_text(json.dumps(report, indent=2))

    # `ok is None` means SKIPPED, not failed: a transition preset cannot be screenshotted here
    # and is covered by transition-proof.py. Counting it as blank put "2 blank preset(s)" on a
    # completely clean run, which is the kind of false alarm that teaches people to ignore the
    # summary line.
    skipped_presets = [p for p in report["presets"] if p.get("ok") is None]
    bad_presets = [p for p in report["presets"] if p.get("ok") is False]
    checks = report["checks"]
    print(f"proof: checks_armed={report['checks_armed']} "
          f"({report['presets_inspected']} pack preset(s) incl. install-probe, "
          f"{checks['passed']}/{checks['armed']} passed, {checks['expected']} expected); "
          f"{len(bad_presets)} blank preset(s); "
          f"{len(skipped_presets)} not renderable here; {len(report['warnings'])} warning(s)")
    for p in report["presets"]:
        if p.get("ok") is None:
            print(f"  {p['pack']}/{p['id']:<14} {p['kind']:<10} {p.get('skipped', 'not rendered here')}")
            continue
        print(f"  {p['pack']}/{p['id']:<14} {p['kind']:<10} nonblank={p['nonblank_ratio']:.3f} "
              f"hue={p['dominant_hue']:.0f} {'OK' if p['ok'] else 'BLANK'}")
    for w in report["warnings"]:
        print("  WARN", w)

    if report["checks_armed"] == 0:
        print("proof: NOTHING INSPECTED")
        return 2
    if (bad_presets or checks["passed"] < checks["armed"] or checks["armed"] != checks["expected"]
            or report["warnings"]):
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--plugin-build", required=True, help="path to the built engine repo (has install-local.sh)")
    ap.add_argument("--pack", help="path to a pack dir (pack.json + effects/) to install and enumerate")
    ap.add_argument("--out", default="build_x86_64/proof", help="directory for report.json, screenshots, obs.log")
    ap.add_argument("--keep", action="store_true", help="keep the temp OBS sandbox and print its path")
    args = ap.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
