#!/usr/bin/env python3
"""Do both plugins actually LOAD, and does each work with the other absent?

"Installable on its own" is a claim. A plugin that builds and never loads -- a missing symbol, a
bad module export -- looks identical from the build log, and a shared static core is exactly the
change that can break one plugin's module identity while leaving the other's intact.

Three OBS boots: both installed, the visualizer alone, the alerts plugin alone. Each asserts the
plugin's own load line is in the log AND that its source kinds are registered.
"""
import asyncio, os, subprocess, sys, tempfile, shutil
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ff_proof  # noqa: E402
import proof  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
FAILS, CHECKS = [], []

def check(name, ok, detail):
    CHECKS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok: FAILS.append(name)

async def kinds():
    ws, c = await ff_proof.open_client("loadcheck")
    try:
        for _ in range(60):
            try:
                await c.request("GetSceneList"); break
            except Exception: await asyncio.sleep(1)
        r = await c.request("GetInputKindList")
        return set(r["inputKinds"])
    finally:
        await ws.close()

def boot(which):
    cfg = Path(tempfile.mkdtemp(prefix="ff-load-")); obs_cfg = cfg / "obs-studio"; obs_cfg.mkdir(parents=True)
    proof.write_ws_config(obs_cfg)
    subprocess.run([str(REPO / "install-local.sh"), str(obs_cfg)],
                   env=dict(os.environ, FOXFIRE_PLUGINS=which), check=True,
                   stdout=subprocess.DEVNULL)
    proof.wait_for_port_free(proof.PORT)  # never connect to a previous boot's dying OBS
    p = subprocess.Popen(["xvfb-run","-a","-s",f"-screen 0 {proof.SCREEN}","obs","--multi","--minimize-to-tray"],
                         env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
    try:
        proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
        got = asyncio.run(kinds())
    finally:
        proof.terminate_process_group(p)
    logs = sorted(obs_cfg.glob("logs/*.txt"))
    log = logs[-1].read_text(errors="replace") if logs else ""
    shutil.rmtree(cfg, ignore_errors=True)
    return got, log

print("--- both plugins installed ---")
got, log = boot("obs-foxfire obs-foxfire-alerts")
check("the visualizer's source kind is registered", "foxfire_visualizer" in got, f"kinds include foxfire_visualizer: {'foxfire_visualizer' in got}")
check("the alerts plugin loads", "alerts: plugin loaded successfully" in log, "its own load line is in obs.log")
check("the alerts plugin resolves a text source at load", "alerts: drawing text with 'text_ft2_source_v2'" in log,
      "it names the kind it picked, so the answer is in the log before anyone adds a source")

print("--- visualizer alone (the alerts plugin NOT installed) ---")
got, log = boot("obs-foxfire")
check("the visualizer still works with the alerts plugin absent", "foxfire_visualizer" in got, f"{'foxfire_visualizer' in got}")
check("and nothing from the alerts plugin is in the log", "alerts: plugin loaded" not in log, "no alerts lines")

print("--- alerts alone (the visualizer NOT installed) ---")
got, log = boot("obs-foxfire-alerts")
check("the alerts plugin loads with the visualizer absent", "alerts: plugin loaded successfully" in log,
      "this is the claim 'installable on its own' actually means")
check("and the visualizer's source kind is gone", "foxfire_visualizer" not in got,
      f"kinds include foxfire_visualizer: {'foxfire_visualizer' in got} (must be False, or they were never separate)")

print(f"\nplugin-split proof: {len(CHECKS)-len(FAILS)}/{len(CHECKS)} passed")
if not CHECKS:
    print("plugin-split proof: NOTHING INSPECTED")
    raise SystemExit(2)
raise SystemExit(1 if FAILS else 0)
