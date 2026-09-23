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


def write_minimal_collection(obs_cfg: Path) -> None:
    """One empty scene. The proof creates its own source in-process, so nothing is needed here
    beyond a profile OBS will boot with."""
    d = obs_cfg / "basic" / "scenes"
    d.mkdir(parents=True, exist_ok=True)
    (d / "Untitled.json").write_text(
        '{"name":"Untitled","current_scene":"A","current_program_scene":"A",'
        '"scene_order":[{"name":"A"}],"sources":[{"name":"A","id":"scene",'
        '"versioned_id":"scene","settings":{"items":[]}}],"transitions":[],"groups":[],'
        '"quick_transitions":[],"saved_projectors":[],"canvases":[],'
        '"resolution":{"x":640,"y":360},"version":2,"modules":{}}\n')
    p = obs_cfg / "basic" / "profiles" / "Untitled"
    p.mkdir(parents=True, exist_ok=True)
    (p / "basic.ini").write_text(
        "[General]\nName=Untitled\n"
        "[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSCommon=30\n")
    (obs_cfg / "global.ini").write_text(
        "[Basic]\nProfile=Untitled\nProfileDir=Untitled\nSceneCollection=Untitled\n"
        "SceneCollectionFile=Untitled\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    ap.add_argument("--seconds", type=float, default=25.0,
                    help="how long to let OBS run before reading the log")
    a = ap.parse_args()

    cfg = Path(tempfile.mkdtemp(prefix="ff-dock-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    try:
        proof.install_plugin(Path(a.plugin_build).resolve(), obs_cfg)
        write_minimal_collection(obs_cfg)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi",
             "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg), FOXFIRE_PROC_PROOF="1"),
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
    print(f"\ndock proof: {passed}/{total} passed")
    if total < EXPECTED_CHECKS:
        print(f"dock proof: only {total} check(s) ran against the {EXPECTED_CHECKS} this harness "
              f"is meant to make -- a check has gone missing")
        return 2
    return 0 if passed == total else 1


if __name__ == "__main__":
    raise SystemExit(main())
