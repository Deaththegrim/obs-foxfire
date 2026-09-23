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
METER_RECT = re.compile(r"dock-grab: meter (\d+) at (-?\d+),(-?\d+) (\d+)x(\d+)")
PICK_ASK = re.compile(r"dock-pick: asked for (pack|preset) '([^']*)'")
PICK_WAS = re.compile(r"dock-pick: source was on '([^']*)' / '([^']*)'")
PICK_GOT = re.compile(r"dock-pick: source now on '([^']*)' / '([^']*)'")
PICK_ERR = re.compile(r"dock-pick: (no rows|'[^']*' is not in the (?:preset|pack) list"
                      r"|the row went away before the write landed)")

# The scene collection starts on this pack/preset and the dock is asked to switch to the other.
# Both presets are `basics` visualizer presets; the switch proves nothing if they are the same
# string, and the harness asserts the source really STARTED on PICK_FROM rather than assuming it.
PICK_PACK = "basics"
PICK_FROM = "bars"
PICK_TO = "wave"
# The pack half needs a second pack that also has a visualizer preset, and one whose preset ids do
# NOT overlap basics' -- the whole point is to land on a pack that cannot keep "bars".
PICK_PACK_TO = "ring"


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
               {"pack": PICK_PACK, "preset": PICK_FROM, "width": 640, "height": 360, "audio_mode": 0})
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


def _find_pack(name: str):
    """The packs repo lives beside this one and is private.

    It used to be a nicety -- the dock lists a SOURCE, not a rendered preset, so it listed one
    either way. It is a requirement now: every switch check below drives a combo that the packs
    fill, so without them the combo is empty, the pick is refused and the dock half cannot run at
    all. Missing packs are therefore a SKIP with a non-zero exit unless --allow-skips says the
    caller knows, never a quiet pass."""
    here = Path(__file__).resolve().parent.parent
    for c in (here.parent / "foxfire" / "packs" / name,
              Path.home() / "vault" / "projects" / "foxfire" / "packs" / name):
        if c.is_dir():
            return c
    return None


def _build_has_dock(plugin_build: Path) -> bool:
    """Whether the BUILT plugin contains a dock, read off the artefact rather than guessed.

    The harness has to run against both a Qt build and a Qt-off one, and it used to decide which by
    whether the dock's own registration line turned up in the log -- inferring the build from the
    absence of the very thing it exists to check. Every real failure (registration refused, the
    frontend event never fired, FF_HAVE_DOCK not defined) looked exactly like "ENABLE_QT is off"
    and exited 0. The dock id is a string constant in ff-dock.cpp, so it is in the .so when the
    dock is compiled in and absent when it is not."""
    for so in plugin_build.rglob("obs-foxfire.so"):
        try:
            return b"foxfire_dock" in so.read_bytes()
        except OSError:
            continue
    return False


# MeterWidget paints its bars in this colour (src/ff-dock.cpp). Counting them is how the grab
# distinguishes a meter that DREW from one that did not -- pixel variance cannot, because the row's
# own labels supply plenty of it either way.
#
# Counted ONLY inside the meter rects the dock logs, and that restriction is load-bearing: this is
# also the colour ff-dock.cpp gives the status label on a fault ("color: #ff6a4d" -- the same
# 255,106,77). Counted over the whole grab, a 256-character refusal rendered in red clears any
# sensible bar threshold on its own, so a MeterWidget::paintEvent that drew nothing passed
# whenever a row was faulted -- which is precisely when the meter is worth checking.
BAR_RGB = (255, 106, 77)


def _measure(png: Path, rects):
    """(w, h, pixel std, bar pixels inside `rects`) of the dock's self-grab, or None.

    numpy/PIL are already a hard dependency of every other proof in this directory."""
    if not png.is_file():
        return None
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(png).convert("RGB"), dtype=float)
    near = (np.abs(a - np.array(BAR_RGB, dtype=float)) <= 12).all(axis=2)
    img_h, img_w = a.shape[0], a.shape[1]
    inside = np.zeros((img_h, img_w), dtype=bool)
    for (x, y, w, h) in rects:
        x0, y0 = max(x, 0), max(y, 0)
        x1, y1 = min(x + w, img_w), min(y + h, img_h)
        if x1 > x0 and y1 > y0:
            inside[y0:y1, x0:x1] = True
    return a.shape[1], a.shape[0], float(a.std()), int((near & inside).sum())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    ap.add_argument("--seconds", type=float, default=25.0,
                    help="how long to let OBS run before reading the log")
    ap.add_argument("--grab-out", type=Path,
                    help="also keep the dock's self-grab here, to look at")
    ap.add_argument("--allow-skips", action="store_true",
                    help="exit 0 when a half had to be skipped for missing packs")
    a = ap.parse_args()

    build = Path(a.plugin_build).resolve()
    has_dock = _build_has_dock(build)
    packs = {n: _find_pack(n) for n in (PICK_PACK, PICK_PACK_TO)}
    missing = sorted(n for n, p in packs.items() if p is None)

    def boot(extra_env: dict, want_grab: bool):
        """One headless OBS run. Returns (log text, grab stats, exit status)."""
        cfg = Path(tempfile.mkdtemp(prefix="ff-dock-"))
        obs_cfg = cfg / "obs-studio"
        obs_cfg.mkdir(parents=True)
        try:
            proof.install_plugin(build, obs_cfg)
            for p in packs.values():
                if p:
                    proof.install_pack(p, obs_cfg)
            write_minimal_collection(obs_cfg)
            grab = cfg / "dock.png"
            env = dict(os.environ, XDG_CONFIG_HOME=str(cfg), FOXFIRE_PROC_PROOF="1", **extra_env)
            if want_grab:
                env["FOXFIRE_DOCK_GRAB"] = str(grab)
            p = subprocess.Popen(
                ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi",
                 "--minimize-to-tray"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                preexec_fn=os.setsid)
            try:
                time.sleep(a.seconds)
            finally:
                proof.terminate_process_group(p)
            logs = sorted(obs_cfg.glob("logs/*.txt"))
            text = logs[-1].read_text(errors="replace") if logs else ""
            rects = [(int(x), int(y), int(w), int(h))
                     for _, x, y, w, h in METER_RECT.findall(text)]
            stats = _measure(grab, rects) if want_grab else None
            if want_grab and a.grab_out and grab.is_file():
                a.grab_out.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(grab, a.grab_out)
            return text, stats, rects, p.returncode
        finally:
            shutil.rmtree(cfg, ignore_errors=True)

    text, grab_stats, meter_rects, rc = boot({"FOXFIRE_DOCK_PICK": PICK_TO}, want_grab=True)
    if not text:
        print("dock proof: OBS wrote no log at all")
        return 2

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
    if not has_dock:
        # Derived from the .so, not from the log. A build genuinely without a dock is the only
        # thing that skips here, and a dock that IS compiled in but produced no registration line
        # falls through to the check below and fails, instead of being excused as "Qt off".
        print(f"\ndock proof: {passed}/{total} tap checks passed; the built plugin contains no "
              f"dock (ENABLE_QT off) -- dock half not checked")
        return 0 if passed == total else 1
    if not reg:
        print(f"\ndock proof: the built plugin CONTAINS a dock but it never registered -- no "
              f"'dock: registered=' line. The frontend event did not fire, or the widget was "
              f"refused. {passed}/{total} tap checks passed.")
        return 2

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
    # How much of the grab the bar count below is allowed to look at. Without at least one rect the
    # count is taken over nothing and 0 is indistinguishable from a meter that did not paint, so
    # the gate has to state its own coverage rather than quietly inspecting an empty region.
    dcheck("and reports where its meters are", len(meter_rects) >= 1,
           f"{len(meter_rects)} meter rect(s): {meter_rects}")
    if grab_stats:
        w, h, std, bars = grab_stats
        # 0.00 is what an empty widget of the same size measures; a laid-out one measured 39-41.
        dcheck("and the picture is not a blank panel", std > 5.0, f"{w}x{h} pixel std {std:.2f}")
        # The meters specifically. Before the grab the dock seeds every meter with a known ramp
        # (see SourceRow::seedTestFrame): headless OBS has no audio, a meter reading silence is
        # legitimately flat, and a meter that paints NOTHING is flat in the same way -- measured, a
        # MeterWidget::paintEvent that returns immediately passed the std check above with room to
        # spare, because the labels carry the variance. Counting the bar colour INSIDE the meter
        # rects is what tells them apart; see BAR_RGB for why the restriction matters.
        dcheck("and the meters actually drew their bars", bars > 50 and len(meter_rects) >= 1,
               f"{bars} pixel(s) of the bar colour {BAR_RGB} inside {len(meter_rects)} meter rect(s)")
    else:
        dcheck("and the picture is not a blank panel", False, "no PNG was produced")
        dcheck("and the meters actually drew their bars", False, "no PNG was produced")

    # The preset switch -- the thing junkie asked for by name, and the one behaviour no picture can
    # show: a grab cannot tell that an instance RELOADED. The dock drives its own combo exactly as a
    # click does and logs what it asked for against what the source ended up with.
    def _check_exit(which: str, code):
        """How OBS went down. The harness kills the process group with SIGTERM, so the expected
        status is -SIGTERM (or -SIGKILL if it needed the harder push, or a plain 0 if it managed
        to exit first). Anything else is a fault signal -- and the failure mode this run is most
        likely to produce is a crash at UNLOAD, after every log line the checks read has already
        been written. Without this the harness reads a perfect log off a process that segfaulted."""
        import signal as _sig
        ok = code in (0, None, -_sig.SIGTERM, -_sig.SIGKILL)
        dcheck(f"and {which} came down cleanly", ok,
               "still running" if code is None else
               (f"exit {code}" if code >= 0 else f"killed by {_sig.Signals(-code).name}"))

    def check_pick(log: str, label: str, want_pack: str, want_preset: str, moved: str):
        """One switch, judged on what the SOURCE was on before and after.

        `moved` names the field that must have changed. The previous version compared the value
        the harness had just put in the environment against another module constant, so it read
        `"wave" != "bars"` and could not fail; the before-line the dock now logs is what makes it
        a real question."""
        err = PICK_ERR.search(log)
        ask, was, got = PICK_ASK.search(log), PICK_WAS.search(log), PICK_GOT.search(log)
        if err:
            dcheck(f"{label} switches the source", False, err.group(1))
            dcheck(f"{label}: it was not already there", False, "no switch happened")
            return
        if not ask or not was or not got:
            dcheck(f"{label} switches the source", False,
                   f"the dock never reported a whole pick -- asked={bool(ask)} "
                   f"was={bool(was)} now={bool(got)}")
            dcheck(f"{label}: it was not already there", False, "no switch happened")
            return
        dcheck(f"{label} switches the source",
               got.group(1) == want_pack and got.group(2) == want_preset,
               f"asked for {ask.group(1)} '{ask.group(2)}'; source went "
               f"'{was.group(1)}/{was.group(2)}' -> '{got.group(1)}/{got.group(2)}', "
               f"wanted '{want_pack}/{want_preset}'")
        # A source that was already on the target makes the check above pass for the wrong reason.
        before = was.group(1) if moved == "pack" else was.group(2)
        after = got.group(1) if moved == "pack" else got.group(2)
        dcheck(f"{label}: it was not already there", before != after,
               f"{moved} went '{before}' -> '{after}'")

    # Both switch halves drive combos the PACKS fill. Without them the combo is empty and every
    # pick is refused, so they skip together -- loudly, and only when the caller has said it knows.
    if PICK_PACK not in missing:
        check_pick(text, "clicking a preset", PICK_PACK, PICK_TO, "preset")
    elif not a.allow_skips:
        dcheck("clicking a preset switches the source", False,
               f"pack '{PICK_PACK}' not found -- pass --allow-skips if that is expected")
        dcheck("clicking a preset: it was not already there", False, "not run")
    else:
        print(f"  [SKIP] the preset switch: pack '{PICK_PACK}' not found")

    # The PACK half, in its own boot. It takes a different path through apply(): the pack is
    # written, the preset list is rebuilt for the new pack, and the source then has to end up on a
    # preset that EXISTS in it. Nothing gated that before, and it is the path that leaves the
    # canvas blank when it goes wrong -- ff_instance_update answers a preset that is not in the
    # pack by loading zero layers.
    skipped = bool(missing)
    if missing and not a.allow_skips:
        dcheck("switching pack lands on a preset the new pack has", False,
               f"pack(s) not found: {', '.join(missing)} -- pass --allow-skips if that is expected")
        dcheck("switching pack: it was not already there", False, "not run")
    elif missing:
        print(f"  [SKIP] the pack switch: pack(s) not found: {', '.join(missing)}")
    else:
        ptext, _, _, prc = boot({"FOXFIRE_DOCK_PICK_PACK": PICK_PACK_TO}, want_grab=False)
        to_dir = packs[PICK_PACK_TO]
        assert to_dir is not None  # `missing` is empty on this branch
        ring_presets = sorted(
            p["id"] for p in json.loads((to_dir / "pack.json").read_text())["presets"]
            if p.get("kind") == "visualizer")
        pask, pwas, pgot = PICK_ASK.search(ptext), PICK_WAS.search(ptext), PICK_GOT.search(ptext)
        perr = PICK_ERR.search(ptext)
        if perr or not (pask and pwas and pgot):
            dcheck("switching pack lands on a preset the new pack has", False,
                   perr.group(1) if perr else "the dock never reported a whole pack pick")
            dcheck("switching pack: it was not already there", False, "no switch happened")
        else:
            landed = pgot.group(2)
            dcheck("switching pack lands on a preset the new pack has",
                   pgot.group(1) == PICK_PACK_TO and landed in ring_presets,
                   f"source went '{pwas.group(1)}/{pwas.group(2)}' -> "
                   f"'{pgot.group(1)}/{landed}'; {PICK_PACK_TO} offers {ring_presets}")
            dcheck("switching pack: it was not already there", pwas.group(1) != pgot.group(1),
                   f"pack went '{pwas.group(1)}' -> '{pgot.group(1)}'")
        _check_exit("the pack-switch run", prc)

    _check_exit("the main run", rc)

    print(f"\ndock proof: {passed}/{total} tap checks, {dock_checks - dock_fails}/{dock_checks} "
          f"dock checks{' (pack switch SKIPPED)' if skipped else ''}")
    return 0 if (passed == total and dock_fails == 0) else 1


if __name__ == "__main__":
    raise SystemExit(main())
