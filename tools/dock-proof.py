#!/usr/bin/env python3
"""The per-source meter tap, proved against a real obs_source_t inside a real OBS.

`ff-props.c` registers two procedures on every Foxfire source -- `ff_meter_read` and
`ff_dock_status` -- so anything holding an `obs_source_t *` can read that instance's live analysis
and state. That is what lets a dock draw meters without the plugin growing a global registry of
live instances, and it is the first use of a proc handler anywhere in this project.

Nothing else can check it:

  * the unit tests cannot -- `ff_instance_create` calls `obs_enter_graphics`, so there is no
    instance to call a procedure on outside a running OBS;
  * obs-websocket cannot -- it has no generic "call this proc handler" request, and adding a
    websocket vendor request purely to make this scriptable would be a second plugin API surface
    for coverage this gets for nothing.

So the plugin proves it about itself: `FOXFIRE_PROC_PROOF=1` makes `plugin-main.c` create a real
source on the first tick, call both procedures, and log a PASS/FAIL line per check. This boots OBS
headless and reads the verdict back out of the log.

It is a GATE, not a preview. A run that produces no verdict line at all fails -- the whole failure
mode this is guarding against is a tap that is silently not there.

    dock-proof.py --plugin-build .
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import proof  # noqa: E402

# Kept in step by hand, the same way tools/proof.py's EXPECTED_CHECKS is: the count is asserted,
# not just printed, because a check deleted or made unreachable by an early return shows up as
# neither a FAIL nor a skip. Raise it when you add one to proc_proof_tick.
EXPECTED_CHECKS = 16

VERDICT = re.compile(r"proc-proof: (\d+)/(\d+) passed")
LINE = re.compile(r"proc-proof: \[(PASS|FAIL)\] (.+)")
DOCK_REG = re.compile(r"dock: registered=(TRUE|FALSE)")
DOCK_ROWS = re.compile(r"dock: (\d+) source\(s\)")
DOCK_GRAB = re.compile(r"dock-grab: (\d+)x(\d+) saved=(TRUE|FALSE)")


def _item(name: str, i: int) -> dict:
    return {"name": name, "source_uuid": "", "visible": True, "locked": False, "rot": 0.0,
            "pos": {"x": 0.0, "y": 0.0}, "scale": {"x": 1.0, "y": 1.0}, "align": 5,
            "bounds_type": 0, "bounds_align": 0, "bounds": {"x": 0.0, "y": 0.0},
            "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0, "id": i,
            "group_item_backup": False, "scale_filter": "disable", "blend_method": "default",
            "blend_type": "normal", "show_transition": {"duration": 0},
            "hide_transition": {"duration": 0}, "private_settings": {}}


def _src(name: str, sid: str, settings: dict) -> dict:
    return {"name": name, "id": sid, "versioned_id": sid, "settings": settings,
            "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5, "enabled": True,
            "muted": False, "push-to-mute": False, "push-to-mute-delay": 0, "push-to-talk": False,
            "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
            "deinterlace_field_order": 0, "monitoring_type": 0, "private_settings": {}}


def write_minimal_collection(obs_cfg: Path) -> None:
    """One scene holding a real Foxfire visualizer.

    The proc-proof half creates its own source in-process, but the DOCK half cannot: the dock only
    ever sees what obs_enum_sources returns, so a collection with no Foxfire source in it proves
    only that the dock can say "nothing here" -- which it renders perfectly well, and which any
    is-it-blank check would pass. The source has to exist before OBS starts.

    Named Untitled, matching transition-proof.py: OBS resolves the collection by that name out of
    global.ini, and a collection named anything else was measured to be ignored entirely ("No scene
    file found, creating default scene") with the dock then correctly reporting zero sources.
    """
    d = obs_cfg / "basic" / "scenes"
    d.mkdir(parents=True, exist_ok=True)
    viz = _src("ff_dock_viz", "foxfire_visualizer",
               {"pack": "basics", "preset": "bars", "width": 640, "height": 360, "audio_mode": 0})
    scene = _src("A", "scene", {"id_counter": 2, "custom_size": False, "items": [_item("ff_dock_viz", 1)]})
    scene["versioned_id"] = "scene"
    (d / "Untitled.json").write_text(json.dumps(
        {"name": "Untitled", "current_scene": "A", "current_program_scene": "A",
         "scene_order": [{"name": "A"}], "sources": [viz, scene], "transitions": [],
         "groups": [], "quick_transitions": [], "saved_projectors": [], "canvases": [],
         "resolution": {"x": 640, "y": 360}, "version": 2, "modules": {}}, indent=2))
    p = obs_cfg / "basic" / "profiles" / "Untitled"
    p.mkdir(parents=True, exist_ok=True)
    (p / "basic.ini").write_text(
        "[General]\nName=Untitled\n"
        "[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSCommon=30\n")
    (obs_cfg / "global.ini").write_text(
        "[Basic]\nProfile=Untitled\nProfileDir=Untitled\nSceneCollection=Untitled\n"
        "SceneCollectionFile=Untitled\n")


def _find_basics():
    """The packs repo lives beside this one and is private; without it the visualizer has no preset
    to load. The dock still lists the source either way -- what it lists is a source, not a
    rendered preset -- so this is a nicety, not a requirement."""
    here = Path(__file__).resolve().parent.parent
    for c in (here.parent / "foxfire" / "packs" / "basics",
              Path.home() / "vault" / "projects" / "foxfire" / "packs" / "basics"):
        if c.is_dir():
            return c
    return None


# MeterWidget paints its bars in this colour (src/ff-dock.cpp). Counting them is how the grab
# distinguishes a meter that DREW from one that did not -- pixel variance cannot, because the row's
# own labels supply plenty of it either way.
BAR_RGB = (255, 106, 77)


def _measure(png: Path):
    """(w, h, pixel std, bar pixels) of the dock's self-grab, or None. numpy/PIL are already a hard
    dependency of every other proof in this directory."""
    if not png.is_file():
        return None
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(png).convert("RGB"), dtype=float)
    near = (np.abs(a - np.array(BAR_RGB, dtype=float)) <= 12).all(axis=2)
    return a.shape[1], a.shape[0], float(a.std()), int(near.sum())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    ap.add_argument("--seconds", type=float, default=25.0,
                    help="how long to let OBS run before reading the log")
    ap.add_argument("--grab-out", type=Path,
                    help="also keep the dock's self-grab here, to look at")
    a = ap.parse_args()

    cfg = Path(tempfile.mkdtemp(prefix="ff-dock-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    try:
        proof.install_plugin(Path(a.plugin_build).resolve(), obs_cfg)
        basics = _find_basics()
        if basics:
            proof.install_pack(basics, obs_cfg)
        write_minimal_collection(obs_cfg)
        grab = cfg / "dock.png"
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi",
             "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg), FOXFIRE_PROC_PROOF="1",
                     FOXFIRE_DOCK_GRAB=str(grab)),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            time.sleep(a.seconds)
        finally:
            proof.terminate_process_group(p)
        logs = sorted(obs_cfg.glob("logs/*.txt"))
        if not logs:
            print("dock proof: OBS wrote no log at all")
            return 2
        text = logs[-1].read_text(errors="replace")
        grab_stats = _measure(grab)
        if a.grab_out and grab.is_file():
            a.grab_out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(grab, a.grab_out)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)

    for kind, name in LINE.findall(text):
        print(f"  [{kind}] {name}")
    m = VERDICT.search(text)
    if not m:
        # The failure this whole file exists for: the procedures are not registered, or the hook
        # never ran. Silence is not a pass.
        print("dock proof: NO VERDICT -- the proc-proof hook produced nothing. Either the tap is "
              "not registered, the plugin did not load, or OBS never ticked.")
        return 2
    passed, total = int(m.group(1)), int(m.group(2))
    if total < EXPECTED_CHECKS:
        print(f"dock proof: only {total} check(s) ran against the {EXPECTED_CHECKS} this harness "
              f"is meant to make -- a check has gone missing")
        return 2

    # ---- the dock half ------------------------------------------------------------------
    # Skipped, loudly, when the build has no dock: the tap above is deliberately NOT gated on Qt,
    # so this harness has to run in both builds and must not report the Qt-off one as if it had
    # checked a widget.
    dock_checks = 0
    dock_fails = 0

    def dcheck(name, ok, detail):
        nonlocal dock_checks, dock_fails
        dock_checks += 1
        if not ok:
            dock_fails += 1
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")

    reg = DOCK_REG.search(text)
    if not reg:
        print(f"\ndock proof: {passed}/{total} tap checks passed; "
              f"no dock in this build (ENABLE_QT off) -- not checked")
        return 0 if passed == total else 1

    dcheck("the dock registers with the OBS frontend", reg.group(1) == "TRUE", f"registered={reg.group(1)}")
    rows = DOCK_ROWS.findall(text)
    n = int(rows[-1]) if rows else -1
    # THE check. A dock showing "No Foxfire sources" renders perfectly well and passes every
    # is-it-blank test there is -- measured: 321x72, pixel std 39.43, 1328 distinct colours, and
    # completely wrong. Pixels cannot tell those apart; this line can.
    dcheck("and finds the Foxfire source in the scene", n >= 1,
           f"{n} source(s) reported" if n >= 0 else "the dock never reported a source count")
    g = DOCK_GRAB.search(text)
    dcheck("and draws itself to a picture", bool(g) and g.group(3) == "TRUE",
           f"{g.group(1)}x{g.group(2)} saved={g.group(3)}" if g else "no grab line")
    if grab_stats:
        w, h, std, bars = grab_stats
        # 0.00 is what an empty widget of the same size measures; a laid-out one measured 39-41.
        dcheck("and the picture is not a blank panel", std > 5.0, f"{w}x{h} pixel std {std:.2f}")
        # The meters specifically. Before the grab the dock seeds every meter with a known ramp
        # (see SourceRow::seedTestFrame): headless OBS has no audio, a meter reading silence is
        # legitimately flat, and a meter that paints NOTHING is flat in the same way -- measured, a
        # MeterWidget::paintEvent that returns immediately passed the std check above with room to
        # spare, because the labels carry the variance. Counting the bar colour is what tells them
        # apart.
        dcheck("and the meters actually drew their bars", bars > 50,
               f"{bars} pixel(s) of the bar colour {BAR_RGB}")
    else:
        dcheck("and the picture is not a blank panel", False, "no PNG was produced")
        dcheck("and the meters actually drew their bars", False, "no PNG was produced")

    print(f"\ndock proof: {passed}/{total} tap checks, {dock_checks - dock_fails}/{dock_checks} dock checks")
    return 0 if (passed == total and dock_fails == 0) else 1


if __name__ == "__main__":
    raise SystemExit(main())
