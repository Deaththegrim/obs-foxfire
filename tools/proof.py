#!/usr/bin/env python3
"""packforge-facing render proof CLI.

Entry point: `python3 tools/proof.py --plugin-build <engine repo> [--pack <pack dir>] [--out <out
dir>] [--keep]`. packforge (a separate tool) runs this and then reads `<out>/report.json`.

This does NOT reimplement the render proof. It boots a throwaway OBS under Xvfb (the same sandbox
recipe tools/render-proof.sh uses), installs the built plugin, then runs tools/ff_proof.py's full
29-check harness against it unchanged -- silence/tone/gap/restore-defaults/pack-install/the
properties-vs-render stress race/destroy, the Foxfire Effects filter proof, the spatial (orientation
+ blur-extent) proof, and the transparency (alpha-convention) proof. That harness's check()/CHECKS/
FAILURES bookkeeping is reused, not copied, for everything below.

When --pack is given, this ALSO copies that pack into the sandbox and, for every preset its
pack.json declares, creates the right OBS object (a foxfire_visualizer input for kind
visualizer/overlay, a color_source_v3 input with a foxfire_effects filter for kind effects) driven
from the harness's own still-live 'tone' audio source, screenshots it, and registers one named
ff_proof.check() per preset so a blank preset fails the run. Without --pack only the unchanged
29-check harness runs, matching tools/render-proof.sh's behaviour exactly.

report.json's "inspected" count is the total number of checks armed (harness + any per-preset
checks) -- the most honest answer to "how much did this gate actually look at" for a proof whose
only unit of work is a named check(). It is 0, and the process exits 2, only when the harness could
not even connect to OBS (a boot failure), not merely when --pack was omitted.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import io
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ff_proof  # noqa: E402  -- the 29-check harness this wraps; see its module docstring

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

PORT = 4460
BOOT_TIMEOUT = 60.0  # seconds to wait for obs-websocket's port to open
SCREEN = "1920x1080x24"


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


def install_pack(pack_dir: Path, obs_cfg: Path) -> None:
    dst = obs_cfg / "plugin_config" / "obs-foxfire" / "packs" / pack_dir.name
    if dst.exists():
        shutil.rmtree(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(pack_dir, dst)


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


async def enumerate_pack(client, pack_dir: Path, out: Path) -> list[dict]:
    """Drives every preset in pack_dir/pack.json through the sandbox's live OBS, reusing the
    harness's 'tone' audio source (still present -- ff_proof.run() creates it and never removes
    it) and its 'ffproof' scene (still the current program scene) so audio-reactive presets
    actually light up. Registers one ff_proof.check() per preset."""
    manifest = json.loads((pack_dir / "pack.json").read_text())
    pack_id = manifest["id"]
    presets: list[dict] = []
    for pr in manifest.get("presets", []):
        name = f"proof-{pack_id}-{pr['id']}"
        settings = {"pack": pack_id, "preset": pr["id"], "audio_mode": 1, "audio_source": "tone",
                    "width": 640, "height": 360}
        if pr["kind"] == "effects":
            # color_source_v3, not color_source -- plain color_source does not resolve on this
            # libobs build (libobs registers three versions of the same id; obs-websocket's
            # GetInputKindList only exposes the versioned name)
            await client.request("CreateInput", {
                "sceneName": "ffproof", "inputName": name, "inputKind": "color_source_v3",
                "inputSettings": {"color": 0xFF404040, "width": 640, "height": 360}})
            await client.request("CreateSourceFilter", {
                "sourceName": name, "filterName": "fx", "filterKind": "foxfire_effects",
                "filterSettings": settings})
        else:  # visualizer / overlay
            await client.request("CreateInput", {
                "sceneName": "ffproof", "inputName": name, "inputKind": "foxfire_visualizer",
                "inputSettings": settings})
        await asyncio.sleep(1.5)
        shot = await client.request("GetSourceScreenshot", {
            "sourceName": name, "imageFormat": "png", "imageWidth": 640, "imageHeight": 360})
        raw = base64.b64decode(shot["imageData"].split(",", 1)[1])
        (out / f"{pack_id}-{pr['id']}.png").write_bytes(raw)
        ratio, hue = analyse(raw)
        ok = ratio > 0.01
        ff_proof.check(f"pack preset {pack_id}/{pr['id']} renders non-blank", ok,
                        f"nonblank_ratio={ratio:.4f}, threshold >0.01, dominant_hue={hue:.1f}")
        presets.append({"pack": pack_id, "id": pr["id"], "kind": pr["kind"],
                         "nonblank_ratio": round(ratio, 4), "dominant_hue": round(hue, 1), "ok": ok})
        await client.request("RemoveInput", {"inputName": name})
    return presets


async def drive(pack_dir: Path | None, out: Path, port: int) -> dict:
    report: dict = {"obs": None, "inspected": 0, "presets": [], "warnings": [], "checks": {}}
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
            presets = await enumerate_pack(client, pack_dir, out)
            report["presets"] = presets
            expected += len(presets)
    except Exception as e:
        report["warnings"].append(f"pack enumeration aborted: {e!r}")
    finally:
        if ws is not None:
            try:
                await asyncio.wait_for(ws.close(), timeout=5)
            except asyncio.TimeoutError:
                pass

    report["inspected"] = len(ff_proof.CHECKS)
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
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    pack_dir = Path(args.pack).resolve() if args.pack else None

    cfg_root = Path(tempfile.mkdtemp(prefix="ff-proof-"))
    obs_cfg = cfg_root / "obs-studio"
    obs_cfg.mkdir(parents=True)
    write_ws_config(obs_cfg)
    install_plugin(repo, obs_cfg)
    if pack_dir is not None:
        install_pack(pack_dir, obs_cfg)

    env = dict(os.environ, XDG_CONFIG_HOME=str(cfg_root))
    proc = subprocess.Popen(
        ["xvfb-run", "-a", "-s", f"-screen 0 {SCREEN}", "obs", "--minimize-to-tray"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)

    report = {"obs": None, "inspected": 0, "presets": [], "warnings": [], "checks": {
        "passed": 0, "armed": 0, "expected": ff_proof.EXPECTED_CHECKS}}
    try:
        wait_for_port(PORT, BOOT_TIMEOUT)
        report = await drive(pack_dir, out, PORT)
    except TimeoutError as e:
        report["warnings"].append(str(e))
    finally:
        terminate_process_group(proc)
        logs = sorted(obs_cfg.glob("logs/*.txt"))
        if logs:
            latest = logs[-1]
            for line in latest.read_text(errors="replace").splitlines():
                if "[obs-foxfire]" in line and ("warning" in line.lower() or "failed" in line.lower()):
                    report["warnings"].append(line.strip())
            shutil.copy(latest, out / "obs.log")
        if args.keep:
            print(f"proof: sandbox kept at {cfg_root}")
        else:
            shutil.rmtree(cfg_root, ignore_errors=True)

    (out / "report.json").write_text(json.dumps(report, indent=2))

    bad_presets = [p for p in report["presets"] if not p["ok"]]
    checks = report["checks"]
    print(f"proof: inspected {report['inspected']} checks ({len(report['presets'])} pack preset(s), "
          f"{checks['passed']}/{checks['armed']} passed, {checks['expected']} expected); "
          f"{len(bad_presets)} blank preset(s); {len(report['warnings'])} warning(s)")
    for p in report["presets"]:
        print(f"  {p['pack']}/{p['id']:<14} {p['kind']:<10} nonblank={p['nonblank_ratio']:.3f} "
              f"hue={p['dominant_hue']:.0f} {'OK' if p['ok'] else 'BLANK'}")
    for w in report["warnings"]:
        print("  WARN", w)

    if report["inspected"] == 0:
        print("proof: NOTHING INSPECTED")
        return 2
    if bad_presets or checks["passed"] < checks["armed"] or report["warnings"]:
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
