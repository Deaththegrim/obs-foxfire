#!/usr/bin/env python3
"""First-render proof and gate for the Foxfire Visualizer source and Effects filter.

Drives a sandboxed OBS over obs-websocket: builds a scene, adds a Foxfire Visualizer, screenshots
it against silence and again against a generated tone, exercises the layer-parameter, Restore
Defaults and pack-install paths, then hammers the properties thread while the video thread renders
and updates. It then adds a Foxfire Effects filter on a flat-colour source and proves the filter
actually reads its target, changes its own settings, and leaves the target untouched once removed.
Every invariant is asserted and every measured number is printed with its unit; the script exits
non-zero on any failed check.

Nothing here may block forever. The defect this gate exists to catch is a deadlock between the
video thread and the UI/RPC thread, and obs-websocket answers keepalives from a thread of its own,
so a hung request would leave an await pending until the heat death of the universe rather than
failing. Every request and every connect therefore runs under a wall clock, and a timeout becomes
a named FAIL, not a traceback. tools/render-proof.sh puts a second clock around the whole driver.

This module is both a CLI (`python3 tools/ff_proof.py [port]`, used by tools/render-proof.sh) and an
importable driver: tools/proof.py imports it to run this exact 31-check harness as the first phase of
its packforge-facing proof, then reuses its Client/check()/CHECKS bookkeeping to add pack-driven
preset checks on the same OBS instance. Nothing here is weakened or duplicated for that reuse --
only main() gained a `port` parameter and the old module-level `asyncio.run(main())` + exit call
moved under `if __name__ == "__main__":` so importing this file has no side effects.
"""
import asyncio
import base64
import json
import math
import shutil
import struct
import sys
import time
import wave
import zipfile
from pathlib import Path

import websockets
from PIL import Image, ImageStat

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_symlink_pack_zip import build as build_symlink_zip  # noqa: E402 -- Critical-1 repro zip

PORT = 4460
URL = f"ws://127.0.0.1:{PORT}"
OUT_SILENT = "/tmp/ff-first-render.png"
OUT_TONE = "/tmp/ff-first-render-tone.png"
OUT_GAP = "/tmp/ff-first-render-gap.png"
OUT_RESTORE = "/tmp/ff-first-render-restore.png"
OUT_STRESS = "/tmp/ff-first-render-stress.png"
OUT_FLT_UNFILTERED = "/tmp/ff-filter-unfiltered.png"
OUT_FLT_FILTERED = "/tmp/ff-filter-filtered.png"
OUT_FLT_DIMMER = "/tmp/ff-filter-dimmer.png"
OUT_FLT_REMOVED = "/tmp/ff-filter-removed.png"
OUT_QUAD_UNFILTERED = "/tmp/ff-filter-quad-unfiltered.png"
OUT_QUAD_FILTERED = "/tmp/ff-filter-quad-filtered.png"
OUT_ALPHA_UNFILTERED = "/tmp/ff-filter-alphahole-unfiltered.png"
OUT_ALPHA_FILTERED = "/tmp/ff-filter-alphahole-filtered.png"
WAV = "/tmp/ff-tone.wav"
ZIP = "/tmp/ff-demo2.zip"
ZIP_SYMLINK = "/tmp/ff-symlink-attack.zip"
ZIP_SYMLINK_CLEANUP = "/tmp/ff-symlink-cleanup-test.zip"
SYMLINK_VICTIM_DIR = "/tmp/ff-symlink-victim"
SYMLINK_CANARY = f"{SYMLINK_VICTIM_DIR}/canary.txt"
QUAD_PNG = "/tmp/ff-quadrant.png"
ALPHAHOLE_PNG = "/tmp/ff-alphahole.png"
W, H = 640, 360
STRESS_ROUNDS = 30
# Seconds. Generous for a local socket, short next to a hang -- but 20 was not generous on a
# 2-core CI runner driving OBS under Xvfb with software GL, where the FIRST request that touches
# the UI thread can sit behind the rest of start-up. Two separate proofs died on it, both only in
# CI and both on the first scene they asked for: alert-proof and user-image-proof, each reported
# as `FFTimeout: CreateScene returns`. open_client's wait_until_serving already absorbs the part
# of start-up that answers nothing at all; this covers the part that answers slowly. A genuine
# deadlock still fails, 25 s later.
REQ_TIMEOUT = 45
# the number of check() calls a full, uninterrupted run makes (run() + run_filter() + run_spatial()
# + run_transparency(); NOT counting the FFTimeout handler's own check(), which is a different,
# additional code path that only runs instead of some of the above). Keep this in sync by hand when
# a check is added or removed -- it exists so a run that times out partway through prints a visibly
# SHRUNKEN armed count next to this constant, instead of silently reporting "N/N (N armed)" for
# whatever smaller N it actually reached.
EXPECTED_CHECKS = 33

FAILURES = []
CHECKS = []  # every check that ran, pass or fail -- "armed" count: see check() below


def check(name, ok, detail):
    """Records one gate. Nothing here prints a verdict it has not measured.

    Every call -- pass or fail -- is counted in CHECKS. A green run that never prints how many
    checks COULD have failed is not evidence of anything: the final summary reports passed/total
    together for exactly that reason (see the bottom of this file)."""
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    CHECKS.append(name)
    if not ok:
        FAILURES.append(name)
    return ok


class FFTimeout(Exception):
    """A step that never came back. Carries what was being attempted, so the FAIL names it."""

    def __init__(self, what):
        super().__init__(what)
        self.what = what


async def guard(what, coro):
    try:
        return await asyncio.wait_for(coro, timeout=REQ_TIMEOUT)
    except asyncio.TimeoutError:
        raise FFTimeout(what) from None


def make_tone(path, hz=110.0, seconds=2.0, rate=48000, amp=0.5):
    """2 s of a pure sine, 16-bit mono, so the analyser sees a stable low-band peak."""
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(int(rate * seconds)):
            v = int(amp * 32767 * math.sin(2 * math.pi * hz * i / rate))
            frames += struct.pack("<h", v)
        w.writeframes(bytes(frames))


def make_quadrant_png(path, w=W, h=H):
    """Hard-edged, asymmetric-in-both-axes test image: opaque white in the top-left quadrant,
    opaque black everywhere else. Unlike a flat colour, this can catch a flipped or transposed
    capture (whichever quadrant ends up brightest names the bug) and, via band_counts/band_pixels
    below, whether glow's blur has any spatial extent at all -- see run_spatial()."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    white = Image.new("RGBA", (w // 2, h // 2), (255, 255, 255, 255))
    img.paste(white, (0, 0))  # paste overwrites -- no alpha blending to muddy the hard edge
    img.save(path)


def make_alphahole_png(path, w=W, h=H):
    """Opaque white left half, fully TRANSPARENT right half -- a real alpha discontinuity (not a
    flat-alpha source like color_source_v3), so filtering it exercises whatever alpha convention
    the capture and the layer stack actually use, and gives run_transparency() a genuinely
    transparent region to assert glow does not paint into. The background is stored red (not
    black) to mirror how PNG exporters and some overlay sources can leave the original colour
    behind under alpha=0."""
    img = Image.new("RGBA", (w, h), (255, 0, 0, 0))
    white = Image.new("RGBA", (w // 2, h), (255, 255, 255, 255))
    img.paste(white, (0, 0))
    img.save(path)


def load_rgba(path):
    return Image.open(path).convert("RGBA")


def quadrant_means(img):
    """Mean luma (0..255) of each screen quadrant. Ordering must survive filtering unchanged --
    top-left brightest before AND after -- or the capture/render flipped or transposed the frame:
    a vertical flip moves the bright quadrant to bottom-left, a horizontal flip to top-right, a
    180-degree rotation to bottom-right."""
    w, h = img.size
    hw, hh = w // 2, h // 2
    boxes = {"tl": (0, 0, hw, hh), "tr": (hw, 0, w, hh), "bl": (0, hh, hw, h), "br": (hw, hh, w, h)}
    return {k: sum(ImageStat.Stat(img.crop(box)).mean[:3]) / 3.0 for k, box in boxes.items()}


def luma_bytes(img):
    """Flat row-major luma bytes, one per pixel. The quadrant image is achromatic (R=G=B
    everywhere a filter hasn't tinted it, and glow-only doesn't tint), so convert("L") is
    numerically the same as (R+G+B)/3 for it, and gives fast C-level indexing for the row/column
    scans below instead of a Python-level loop over getdata()."""
    return img.convert("L").tobytes()


def band_counts(img):
    """(rows, cols) that contain at least one INTERMEDIATE pixel -- strictly between the black and
    white plateaus. 0/0 on the hard-edged source; >0/>0 once glow's blur has spread the edge a few
    px in each direction. This is the check a filter that renders nothing (or a pure pass-through)
    cannot pass, unlike a brightness-ratio check on a flat colour."""
    L = luma_bytes(img)
    w, h = img.size
    rows = sum(1 for y in range(h) if any(8 < v < 247 for v in L[y * w:(y + 1) * w]))
    cols = sum(1 for x in range(w) if any(8 < L[y * w + x] < 247 for y in range(h)))
    return rows, cols


def band_pixels(img, y=None, x=None):
    """COUNT of intermediate-luma pixels on one scan line -- row `y` for the vertical (x=w/2) edge,
    column `x` for the horizontal (y=h/2) edge. Not the width of the contiguous run (the scan line
    only crosses one boundary in this test image, so in practice the two are close, but this
    counts every intermediate pixel on the line, it does not find the run and measure it). This is
    the number that would collapse towards 0 px if uv_size were wrong by orders of magnitude too
    large (the shader's per-texel offset shrinks to sub-pixel) or balloon to nearly the whole frame
    if uv_size were wrong too small (the offset becomes huge) -- exactly Ruling 1's subject, the
    target's size driving the renderer instead of a filter's (nonexistent) width/height settings."""
    L = luma_bytes(img)
    w, _ = img.size
    if y is not None:
        return sum(1 for v in L[y * w:(y + 1) * w] if 8 < v < 247)
    return sum(1 for v in L[x::w] if 8 < v < 247)


def make_pack_zip(path):
    """A second pack, cloned from the bundled demo under a new id, to drive Install pack."""
    src = Path(__file__).resolve().parent.parent / "data" / "packs" / "demo"
    manifest = (src / "pack.json").read_text().replace('"id": "demo"', '"id": "demo2"').replace(
        '"name": "Foxfire Demo"', '"name": "Foxfire Demo Two"')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("demo2/pack.json", manifest)
        for eff in sorted((src / "effects").glob("*.effect")):
            z.write(eff, f"demo2/effects/{eff.name}")


class Client:
    def __init__(self, ws):
        self.ws = ws
        self.id = 0

    async def _exchange(self, kind, data):
        self.id += 1
        rid = str(self.id)
        await self.ws.send(json.dumps({"op": 6, "d": {"requestType": kind, "requestId": rid,
                                                      "requestData": data or {}}}))
        while True:
            msg = json.loads(await self.ws.recv())
            if msg["op"] == 7 and msg["d"]["requestId"] == rid:
                status = msg["d"]["requestStatus"]
                if not status["result"]:
                    raise RuntimeError(f"{kind} failed: {status}")
                return msg["d"].get("responseData") or {}

    async def request(self, kind, data=None):
        return await guard(f"{kind} returns", self._exchange(kind, data))


async def open_client(what):
    ws = await guard(f"{what} connects", websockets.connect(URL, max_size=None))
    hello = json.loads(await guard(f"{what} receives Hello", ws.recv()))
    assert hello["op"] == 0, hello
    await ws.send(json.dumps({"op": 1, "d": {"rpcVersion": 1}}))
    ident = json.loads(await guard(f"{what} is identified", ws.recv()))
    assert ident["op"] == 2, ident
    client = Client(ws)
    await wait_until_serving(client, what)
    return ws, client


async def wait_until_serving(client, what, timeout=60.0, attempt=5.0):
    """Block until obs-websocket actually answers a request, not merely a handshake.

    Hello and Identify come back while OBS is still loading plugins and the scene
    collection, so an identified connection is NOT a ready one. The first real request
    then spends the whole REQ_TIMEOUT on a cold runner and the proof dies at
    whatever it happened to ask for first -- seen in CI once as
    `FFTimeout: CreateScene returns`, with the two checks after it reporting FAIL for
    work that never ran. Asking something trivial in a retry loop puts the waiting where
    it belongs and names it properly when it really is stuck.

    A reply that arrives after its attempt timed out is harmless: `_exchange` skips any
    message whose requestId is not the one it is waiting for.
    """
    deadline = time.monotonic() + timeout
    while True:
        try:
            await asyncio.wait_for(client._exchange("GetVersion", {}), timeout=attempt)
            return
        except (asyncio.TimeoutError, RuntimeError):
            if time.monotonic() >= deadline:
                raise FFTimeout(f"{what}: obs-websocket identified but served no request "
                                f"in {timeout:.0f}s") from None
            await asyncio.sleep(0.5)


class Shot:
    """One screenshot, in the three pixel units this proof distinguishes.

    any:  alpha > 0     -- anything at all was written
    lit:  alpha > 8     -- visible, glow halo included
    core: alpha > 200   -- the bars themselves; the only unit that tracks bar WIDTH, because the
                           glow layer smears alpha across whatever gap the bars leave
    """

    def __init__(self, b64, path):
        with open(path, "wb") as f:
            f.write(base64.b64decode(b64.split(",", 1)[1]))
        img = Image.open(path).convert("RGBA")
        alpha = list(img.getchannel("A").tobytes())
        self.path = path
        self.size = img.size
        self.any = sum(1 for a in alpha if a > 0)
        self.lit = sum(1 for a in alpha if a > 8)
        self.core = sum(1 for a in alpha if a > 200)

    def __str__(self):
        return (f"any(a>0)={self.any} px, lit(a>8)={self.lit} px, core(a>200)={self.core} px, "
                f"size={self.size[0]}x{self.size[1]} px -> {self.path}")


async def screenshot(c, path):
    r = await c.request("GetSourceScreenshot", {
        "sourceName": "ff", "imageFormat": "png", "imageWidth": W, "imageHeight": H})
    return Shot(r["imageData"], path)


class FilterShot:
    """One screenshot for the filter proof, in mean-luminance terms rather than Shot's alpha
    histogram: the filter proof is about whether glow BRIGHTENS a flat colour, not about how much
    area got lit, so the unit that matters here is mean(R,G,B) over every pixel, 0..255.

    mean:     average of (R+G+B)/3 across every pixel -- what "brighter" is measured in
    variance: population variance of that per-pixel luminance -- near 0 for a flat colour source
    min/max_alpha: opacity range -- a filter that punches a hole in the target would show here
    """

    def __init__(self, b64, path):
        with open(path, "wb") as f:
            f.write(base64.b64decode(b64.split(",", 1)[1]))
        img = Image.open(path).convert("RGBA")
        # getchannel(...).tobytes(), same idiom Shot above uses for alpha -- not the deprecated
        # getdata() -- one flat bytes object per channel, indexed together below
        r = img.getchannel("R").tobytes()
        g = img.getchannel("G").tobytes()
        b = img.getchannel("B").tobytes()
        a = img.getchannel("A").tobytes()
        n = len(a)
        lumas = [(r[i] + g[i] + b[i]) / 3.0 for i in range(n)]
        self.path = path
        self.size = img.size
        self.mean = sum(lumas) / n
        self.variance = sum((v - self.mean) ** 2 for v in lumas) / n
        self.min_alpha = min(a)
        self.max_alpha = max(a)

    def __str__(self):
        return (f"mean_luma={self.mean:.2f}/255, variance={self.variance:.2f}, "
                f"alpha=[{self.min_alpha}..{self.max_alpha}], size={self.size[0]}x{self.size[1]} px "
                f"-> {self.path}")


async def screenshot_named(c, source_name, path):
    r = await c.request("GetSourceScreenshot", {
        "sourceName": source_name, "imageFormat": "png", "imageWidth": W, "imageHeight": H})
    return FilterShot(r["imageData"], path)


async def stress(name):
    """C1's path: the UI/RPC thread rebuilding properties (which rescans packs and walks the
    renderer's layer array) while the video thread runs update() and renders. Two extra
    connections, so the two request streams genuinely overlap instead of queueing behind one
    another."""
    wa, ca = await open_client("stress reader")
    wb, cb = await open_client("stress writer")
    try:
        async def hammer_props():
            for _ in range(STRESS_ROUNDS):
                await ca.request("GetInputPropertiesListPropertyItems",
                                 {"inputName": name, "propertyName": "preset"})

        async def hammer_settings():
            for i in range(STRESS_ROUNDS):
                await cb.request("SetInputSettings", {
                    "inputName": name, "inputSettings": {"l0.gap": 0.2 if i % 2 else 0.55}})

        await asyncio.gather(hammer_props(), hammer_settings())
    finally:
        await wa.close()
        await wb.close()


async def run(c):
    await c.request("GetVersion")
    await c.request("CreateScene", {"sceneName": "ffproof"})
    await c.request("SetCurrentProgramScene", {"sceneName": "ffproof"})
    await c.request("CreateInput", {
        "sceneName": "ffproof", "inputName": "ff", "inputKind": "foxfire_visualizer",
        "inputSettings": {"pack": "demo", "preset": "bars", "width": W, "height": H,
                          "audio_mode": 0},
        "sceneItemEnabled": True})
    await asyncio.sleep(1.5)

    silent = await screenshot(c, OUT_SILENT)
    print(f"silence (master mix, nothing playing): {silent}")
    check("silence renders nothing", silent.core == 0, f"core={silent.core} px, expected 0 px")

    items = await c.request("GetInputPropertiesListPropertyItems",
                            {"inputName": "ff", "propertyName": "preset"})
    names = [i["itemValue"] for i in items["propertyItems"]]
    check("preset list offers bars", "bars" in names, f"{len(names)} item(s): {names}")

    await c.request("CreateInput", {
        "sceneName": "ffproof", "inputName": "tone", "inputKind": "ffmpeg_source",
        "inputSettings": {"local_file": WAV, "is_local_file": True, "looping": True},
        "sceneItemEnabled": True})
    await asyncio.sleep(1.0)
    await c.request("SetInputSettings", {
        "inputName": "ff", "inputSettings": {"audio_mode": 1, "audio_source": "tone"}})
    await asyncio.sleep(1.5)

    tone = await screenshot(c, OUT_TONE)
    print(f"tone (110 Hz sine via 'tone'): {tone}")
    check("tone lights the bars", tone.core > 5000, f"core={tone.core} px, threshold >5000 px")

    # "l0.gap" is the bars shader's own knob, reached only through ff_instance_update ->
    # ff_renderer_apply_settings. Widening the gap must thin the bars: core is the unit that
    # measures it, because the glow layer keeps `lit` almost unchanged.
    await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"l0.gap": 0.55}})
    await asyncio.sleep(1.0)
    gap = await screenshot(c, OUT_GAP)
    ratio = gap.core / tone.core if tone.core else 0.0
    print(f"gap 0.2 -> 0.55: {gap}")
    check("widening the gap thins the bars", 0.35 <= ratio <= 0.65,
          f"core ratio={ratio:.3f} (={gap.core}/{tone.core} px), window 0.35..0.65, "
          f"shader predicts (1-0.55)/(1-0.2)=0.563")

    # Restore Defaults: obs-websocket's overlay:false is obs_source_reset_settings, the same
    # obs_data_clear + update the button does. With no user value for "l0.gap" the renderer must
    # put the PRESET's 0.2 back, not leave the last user value in place. The window is wide
    # because the analyser's envelope keeps moving between shots, not because the knob is fuzzy.
    await c.request("SetInputSettings", {
        "inputName": "ff", "overlay": False,
        "inputSettings": {"pack": "demo", "preset": "bars", "width": W, "height": H,
                          "audio_mode": 1, "audio_source": "tone"}})
    await asyncio.sleep(1.5)
    restored = await screenshot(c, OUT_RESTORE)
    back = restored.core / tone.core if tone.core else 0.0
    print(f"after Restore Defaults: {restored}")
    check("restore defaults returns the knob to the preset value", 0.85 <= back <= 1.15,
          f"core ratio vs the gap-0.2 tone frame={back:.3f} (={restored.core}/{tone.core} px), "
          f"window 0.85..1.15; it was {gap.core} px at gap 0.55")

    # The Install pack picker's modified callback only fires when the properties are built, which
    # obs_source_properties does via obs_properties_apply_settings -- so setting the path and then
    # asking for a property's items drives the real trigger path.
    await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"install_zip": ZIP}})
    packs = await c.request("GetInputPropertiesListPropertyItems",
                            {"inputName": "ff", "propertyName": "pack"})
    ids = [i["itemValue"] for i in packs["propertyItems"]]
    left = (await c.request("GetInputSettings", {"inputName": "ff"}))["inputSettings"].get("install_zip")
    check("install adds demo2", "demo2" in ids, f"{len(ids)} pack(s): {ids}")
    check("install path cleared one-shot", left in (None, ""), f"install_zip={left!r}")

    # Critical-1 (whole-branch review): a zip containing a symlink entry that points OUTSIDE the
    # packs tree must be refused by the real, currently-loaded plugin, end to end -- not just by
    # the isolated entry-check logic (see tools/build_symlink_pack_zip.py + the before/after repro
    # in the fix report). victim_dir stands in for anything outside packs/ a malicious/careless
    # pack author's symlink could point at; canary.txt proves it, not just the install's own report.
    # Two zips are tested: one with valid manifest (exercises the symlink listing defence directly),
    # and one with invalid min_engine (exercises the cleanup path that must handle symlinks post-extraction).
    victim_dir = Path(SYMLINK_VICTIM_DIR)
    if victim_dir.exists():
        if victim_dir.is_symlink():
            victim_dir.unlink()
        else:
            shutil.rmtree(victim_dir)
    victim_dir.mkdir(parents=True)
    canary = Path(SYMLINK_CANARY)
    canary.write_text("canary-do-not-delete")

    # First test: symlink with valid manifest -- the listing-step refusal must catch it
    build_symlink_zip(ZIP_SYMLINK, str(victim_dir), "evilpack", "0.0.0")
    await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"install_zip": ZIP_SYMLINK}})
    packs_after_attack = await c.request("GetInputPropertiesListPropertyItems",
                                         {"inputName": "ff", "propertyName": "pack"})
    ids_after_attack = [i["itemValue"] for i in packs_after_attack["propertyItems"]]
    # Check both that pack was refused AND that the reason was symlinks (not min_engine)
    check("symlink-zip install refused: pack not added", "evilpack" not in ids_after_attack,
          f"{len(ids_after_attack)} pack(s): {ids_after_attack}")
    # There used to be a check here asserting the refusal REASON by reading "install_msg" out of
    # GetInputSettings. It could never pass: install_msg is an instance field surfaced as a
    # read-only info property (src/ff-props.c add_status_lines, key "status.install"), never a
    # setting, so the read always returned "". That is the mirror of a test that cannot fail --
    # a gate permanently red for a reason unrelated to the thing it names.
    # The reason IS asserted, in tools/proof.py: "pack install: zip contains a symlink entry,
    # refused:" is in ALLOWED_WARNING_TEXTS and that list is now checked for having FIRED, so a
    # refusal for the wrong reason fails the run there.
    check("symlink-zip install: victim directory outside packs/ survives", canary.exists(),
          f"{canary} exists={canary.exists()}")

    # Second test: symlink with invalid min_engine -- exercises the cleanup path (remove_recursive with symlinks)
    build_symlink_zip(ZIP_SYMLINK_CLEANUP, str(victim_dir), "evilpack2", "99.0.0")
    await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"install_zip": ZIP_SYMLINK_CLEANUP}})
    packs_after_cleanup_test = await c.request("GetInputPropertiesListPropertyItems",
                                               {"inputName": "ff", "propertyName": "pack"})
    ids_after_cleanup_test = [i["itemValue"] for i in packs_after_cleanup_test["propertyItems"]]
    check("bad-min_engine zip install refused: pack not added", "evilpack2" not in ids_after_cleanup_test,
          f"{len(ids_after_cleanup_test)} pack(s): {ids_after_cleanup_test}")
    check("cleanup test: victim directory still survives", canary.exists(),
          f"{canary} exists={canary.exists()}")

    # C1: properties (pack rescan + a walk of the renderer's layer array) on the RPC thread
    # against update()/render on the video thread.
    await stress("ff")
    await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"l0.gap": 0.2}})
    await asyncio.sleep(1.0)
    alive = await c.request("GetVersion")
    settings = (await c.request("GetInputSettings", {"inputName": "ff"}))["inputSettings"]
    survived = [i["inputName"] for i in (await c.request("GetInputList"))["inputs"]]
    check("the properties/update race leaves the source intact",
          "ff" in survived and settings.get("l0.gap") == 0.2 and bool(alive.get("obsVersion")),
          f"{STRESS_ROUNDS} properties rebuilds x {STRESS_ROUNDS} setting writes on 2 "
          f"connections; input present={'ff' in survived}, l0.gap={settings.get('l0.gap')!r} "
          f"(last write 0.2), obsVersion={alive.get('obsVersion')!r}")
    stressed = await screenshot(c, OUT_STRESS)
    print(f"after the race: {stressed}")
    check("still rendering after the race", stressed.core > 5000,
          f"core={stressed.core} px, threshold >5000 px")

    # destroy path: removing the input runs ff_instance_destroy with OBS still live, so a deadlock
    # or a double free shows up as a request that never returns rather than hiding in a
    # SIGTERM-truncated shutdown log. The verdict is measured: the input must be gone from both
    # the input list and the scene, and OBS must still name its version.
    await c.request("RemoveInput", {"inputName": "ff"})
    await asyncio.sleep(1.0)
    after = await c.request("GetVersion")
    inputs = [i["inputName"] for i in (await c.request("GetInputList"))["inputs"]]
    scene = [i["sourceName"]
             for i in (await c.request("GetSceneItemList", {"sceneName": "ffproof"}))["sceneItems"]]
    check("destroy removes the input and leaves OBS healthy",
          "ff" not in inputs and "ff" not in scene and bool(after.get("obsVersion")),
          f"inputs={inputs}, scene items={scene}, obsVersion={after.get('obsVersion')!r}")


async def run_filter(c):
    """Foxfire Effects: a filter on 'fxbase' (a flat grey color_source, still in the 'ffproof'
    scene) proves it actually samples its target -- glow-only's one layer is `c + blur(c)*amount`
    (see data/packs/demo/effects/glow.effect), so on a FLAT colour blur(c) == c and the frame must
    get brighter by exactly (1+amount), not merely change. The 'tone' input from run() is reused as
    the audio tap so audio_mode=1 exercises the same path a real effects filter runs under."""
    print("\n--- filter proof: Foxfire Effects ---")
    # "color_source_v3" not "color_source": libobs registers three versions of the same id and
    # obs-websocket's GetInputKindList (checked against a live sandbox) exposes the versioned name
    await c.request("CreateInput", {
        "sceneName": "ffproof", "inputName": "fxbase", "inputKind": "color_source_v3",
        "inputSettings": {"color": 0xFF404040, "width": W, "height": H},
        "sceneItemEnabled": True})
    await asyncio.sleep(1.0)

    unfiltered = await screenshot_named(c, "fxbase", OUT_FLT_UNFILTERED)
    print(f"fxbase, no filter: {unfiltered}")
    check("unfiltered fxbase is fully opaque", unfiltered.min_alpha == 255 and unfiltered.max_alpha == 255,
          f"alpha=[{unfiltered.min_alpha}..{unfiltered.max_alpha}]")
    check("unfiltered fxbase is flat (low variance)", unfiltered.variance < 4.0,
          f"variance={unfiltered.variance:.3f} luma^2, threshold <4.0")

    await c.request("CreateSourceFilter", {
        "sourceName": "fxbase", "filterName": "fx", "filterKind": "foxfire_effects",
        "filterSettings": {"pack": "demo", "preset": "glow-only", "audio_mode": 1,
                           "audio_source": "tone"}})
    await asyncio.sleep(1.5)

    filtered = await screenshot_named(c, "fxbase", OUT_FLT_FILTERED)
    print(f"fxbase + 'fx' (glow-only, l0.amount=0.8, tone playing): {filtered}")
    check("filter does not change the target's size", filtered.size == (W, H),
          f"size={filtered.size[0]}x{filtered.size[1]} px, expected {W}x{H} px")
    check("filtered frame stays fully opaque", filtered.min_alpha == 255 and filtered.max_alpha == 255,
          f"alpha=[{filtered.min_alpha}..{filtered.max_alpha}]")
    up = filtered.mean / unfiltered.mean if unfiltered.mean else 0.0
    check("glow brightens the frame", up > 1.02,
          f"luminance ratio={up:.4f} (={filtered.mean:.2f}/{unfiltered.mean:.2f} luma), "
          f"threshold >1.02, shader predicts (1+0.8)=1.8")

    # the renderer's own knob, reached only through ff_instance_update -> ff_renderer_apply_settings,
    # same path SetInputSettings exercises for the source in run() above -- proves it for the filter
    await c.request("SetSourceFilterSettings", {
        "sourceName": "fxbase", "filterName": "fx", "filterSettings": {"l0.amount": 0.2}})
    await asyncio.sleep(1.5)
    dimmer = await screenshot_named(c, "fxbase", OUT_FLT_DIMMER)
    down = dimmer.mean / filtered.mean if filtered.mean else 0.0
    print(f"fxbase, l0.amount 0.8 -> 0.2: {dimmer}")
    check("SetSourceFilterSettings reaches the filter (luminance drops)", down < 0.99,
          f"luminance ratio={down:.4f} (={dimmer.mean:.2f}/{filtered.mean:.2f} luma), "
          f"threshold <0.99, shader predicts (1+0.2)/(1+0.8)=0.667")

    flist = await c.request("GetSourceFilterList", {"sourceName": "fxbase"})
    fnames = [f["filterName"] for f in flist["filters"]]
    check("GetSourceFilterList shows the filter", "fx" in fnames, f"filters={fnames}")

    await c.request("RemoveSourceFilter", {"sourceName": "fxbase", "filterName": "fx"})
    await asyncio.sleep(1.0)
    removed = await screenshot_named(c, "fxbase", OUT_FLT_REMOVED)
    back = removed.mean / unfiltered.mean if unfiltered.mean else 0.0
    print(f"fxbase, filter removed: {removed}")
    check("removing the filter restores the original mean (within 1%)", abs(back - 1.0) <= 0.01,
          f"luminance ratio vs unfiltered={back:.4f} (={removed.mean:.2f}/{unfiltered.mean:.2f} luma), "
          f"window 0.99..1.01")
    # a filter that never actually rendered would trivially leave the mean unchanged from
    # unfiltered -- the check above alone can't tell "removed correctly" from "was a no-op all
    # along". Comparing against the last FILTERED (amount=0.8) frame it must move AWAY from closes
    # that gap: removed/filtered = 1/1.8 = 0.5556.
    away = removed.mean / filtered.mean if filtered.mean else 0.0
    check("removing the filter also moves the mean away from the last filtered frame",
          0.52 <= away <= 0.60,
          f"luminance ratio vs the amount=0.8 frame={away:.4f} (={removed.mean:.2f}/{filtered.mean:.2f} "
          f"luma), window 0.52..0.60, shader predicts 1/(1+0.8)=0.5556")

    await c.request("RemoveInput", {"inputName": "fxbase"})


async def run_spatial(c):
    """glow-only's own maths (c + blur(c)*amount, see glow.effect) is IDENTICAL for any blur
    radius, any uv_size and any sampling orientation on a perfectly FLAT source -- blurring a
    uniform colour returns that colour regardless. That is why the flat-colour proof in run_filter
    above cannot distinguish a correctly-oriented, correctly-sized capture from a flipped one, a
    uv_size wrong by orders of magnitude, or a filter that captured no spatial content at all: a
    filter forced to unconditionally skip (see the report's ARMED run) still passes 6 of that
    function's 8 checks, because "brighter" and "still opaque and the right size" are also true of
    the untouched original frame passed straight through. This uses a hard-edged, asymmetric image
    instead and checks the SHAPE of the result."""
    print("\n--- spatial proof: orientation and blur extent ---")
    await c.request("CreateInput", {
        "sceneName": "ffproof", "inputName": "quad", "inputKind": "image_source",
        "inputSettings": {"file": QUAD_PNG}, "sceneItemEnabled": True})
    await asyncio.sleep(1.0)

    r = await c.request("GetSourceScreenshot", {
        "sourceName": "quad", "imageFormat": "png", "imageWidth": W, "imageHeight": H})
    with open(OUT_QUAD_UNFILTERED, "wb") as f:
        f.write(base64.b64decode(r["imageData"].split(",", 1)[1]))
    img0 = load_rgba(OUT_QUAD_UNFILTERED)
    qm0 = quadrant_means(img0)
    rows0, cols0 = band_counts(img0)
    st0 = ImageStat.Stat(img0.convert("L"))
    print(f"quad, no filter: quadrant means tl={qm0['tl']:.1f} tr={qm0['tr']:.1f} bl={qm0['bl']:.1f} "
          f"br={qm0['br']:.1f} luma, band rows={rows0} cols={cols0}, frame mean={st0.mean[0]:.2f} "
          f"var={st0.var[0]:.1f} luma^2 -> {OUT_QUAD_UNFILTERED}")
    check("(a) unfiltered: top-left is the brightest quadrant", qm0["tl"] > max(qm0["tr"], qm0["bl"], qm0["br"]) + 50,
          f"tl={qm0['tl']:.1f}, tr={qm0['tr']:.1f}, bl={qm0['bl']:.1f}, br={qm0['br']:.1f} luma")
    check("(b) unfiltered: no blur band -- edges are hard", rows0 == 0 and cols0 == 0,
          f"band rows={rows0} (of {H}), band cols={cols0} (of {W}), expected 0/0")

    # audio_mode=0 is FF_AUDIO_MASTER, not "off" -- the master mix may still carry 'tone' looping
    # from run()/run_filter() above (it does: this section's own printed tr/bl means are nonzero,
    # not the 0.0 a truly silent frame would show), so `level` and therefore glow's blur radius
    # (3..9 px, see glow.effect) vary run to run depending on where in the tone's envelope a
    # screenshot happens to land. Every check below is written with a window wide enough to hold
    # across that whole range -- in particular the band-width window is (2, 100) px, not a tight
    # band around one predicted radius, for exactly this reason.
    await c.request("CreateSourceFilter", {
        "sourceName": "quad", "filterName": "fx", "filterKind": "foxfire_effects",
        "filterSettings": {"pack": "demo", "preset": "glow-only", "audio_mode": 0}})
    await asyncio.sleep(1.5)

    r = await c.request("GetSourceScreenshot", {
        "sourceName": "quad", "imageFormat": "png", "imageWidth": W, "imageHeight": H})
    with open(OUT_QUAD_FILTERED, "wb") as f:
        f.write(base64.b64decode(r["imageData"].split(",", 1)[1]))
    img1 = load_rgba(OUT_QUAD_FILTERED)
    qm1 = quadrant_means(img1)
    rows1, cols1 = band_counts(img1)
    bp_row = band_pixels(img1, y=H // 4)
    bp_col = band_pixels(img1, x=W // 4)
    st1 = ImageStat.Stat(img1.convert("L"))
    print(f"quad + glow-only (silent): quadrant means tl={qm1['tl']:.1f} tr={qm1['tr']:.1f} "
          f"bl={qm1['bl']:.1f} br={qm1['br']:.1f} luma, band rows={rows1} cols={cols1}, "
          f"band pixels row{H//4}={bp_row} px col{W//4}={bp_col} px, frame mean={st1.mean[0]:.2f} "
          f"var={st1.var[0]:.1f} luma^2 -> {OUT_QUAD_FILTERED}")
    check("(a) filtered: top-left is still the brightest quadrant (no flip/transpose)",
          qm1["tl"] > max(qm1["tr"], qm1["bl"], qm1["br"]) + 50,
          f"tl={qm1['tl']:.1f}, tr={qm1['tr']:.1f}, bl={qm1['bl']:.1f}, br={qm1['br']:.1f} luma")
    # >= 2, not > 0: a single stray pixel (an antialiasing or PNG-decode artifact, say) must not
    # be enough to pass this on its own
    check("(b) filtered: glow spreads a measurable blur band across both edges", rows1 >= 2 and cols1 >= 2,
          f"band rows={rows1} (of {H}), band cols={cols1} (of {W}), expected >=2/>=2")
    check("(b) band pixel count is plausible -- collapses towards 0 or balloons if uv_size is wrong "
          "by orders of magnitude", 2 <= bp_row < 100 and 2 <= bp_col < 100,
          f"row{H//4} band={bp_row} px, col{W//4} band={bp_col} px, window [2, 100) px each")
    # explicit epsilons, not a bare `>`/`<`: makes the claimed DIRECTION visible in the threshold
    # itself rather than implied by "not equal", and rules out a same-to-the-last-bit no-op passing
    # on floating-point noise
    check("(c) filtering raises the frame mean by a measurable amount (glow adds light, doesn't "
          "just redistribute it)", st1.mean[0] > st0.mean[0] * 1.001,
          f"frame mean {st0.mean[0]:.2f} -> {st1.mean[0]:.2f} luma, "
          f"needs > {st0.mean[0] * 1.001:.2f} (+0.1%)")
    check("(c) filtering lowers the frame variance by a measurable amount (blur pulls the two "
          "plateaus together)", st1.var[0] < st0.var[0] * 0.999,
          f"frame variance {st0.var[0]:.1f} -> {st1.var[0]:.1f} luma^2, "
          f"needs < {st0.var[0] * 0.999:.1f} (-0.1%)")

    await c.request("RemoveSourceFilter", {"sourceName": "quad", "filterName": "fx"})
    await c.request("RemoveInput", {"inputName": "quad"})


async def run_transparency(c):
    """I2: the capture blend and the final draw blend must agree on ONE alpha convention (see the
    comments in ff-filter.c) or a translucent edge either bleeds colour it shouldn't (capture side)
    or gets darkened by double-applying alpha (draw side). 'alphahole.png' has a genuine alpha
    discontinuity -- opaque white left half, fully transparent right half, stored with the
    background colour (red) left in the pixel data under alpha=0, mirroring what some PNG
    exporters and overlay sources actually do -- so a region deep inside the transparent half, far
    (>100 px) from the only edge in the image, must stay transparent and colourless after
    filtering: any colour reaching it can only have leaked in through a wrong alpha convention, not
    from the glow halo (glow-only's blur radius here is a handful of px, nowhere near 100)."""
    print("\n--- transparency proof: alpha convention ---")
    await c.request("CreateInput", {
        "sceneName": "ffproof", "inputName": "hole", "inputKind": "image_source",
        "inputSettings": {"file": ALPHAHOLE_PNG}, "sceneItemEnabled": True})
    await asyncio.sleep(1.0)

    r = await c.request("GetSourceScreenshot", {
        "sourceName": "hole", "imageFormat": "png", "imageWidth": W, "imageHeight": H})
    with open(OUT_ALPHA_UNFILTERED, "wb") as f:
        f.write(base64.b64decode(r["imageData"].split(",", 1)[1]))
    img0 = load_rgba(OUT_ALPHA_UNFILTERED)
    region0 = ImageStat.Stat(img0.crop((450, 20, 620, 340)))
    print(f"hole, no filter: far-interior region mean RGBA={[round(v, 2) for v in region0.mean]} "
          f"-> {OUT_ALPHA_UNFILTERED}")
    check("sanity: the test region really is transparent before filtering", region0.mean[3] < 1,
          f"mean alpha={region0.mean[3]:.2f}")

    await c.request("CreateSourceFilter", {
        "sourceName": "hole", "filterName": "fx", "filterKind": "foxfire_effects",
        "filterSettings": {"pack": "demo", "preset": "glow-only", "audio_mode": 0, "l0.amount": 0.8}})
    await asyncio.sleep(1.5)

    r = await c.request("GetSourceScreenshot", {
        "sourceName": "hole", "imageFormat": "png", "imageWidth": W, "imageHeight": H})
    with open(OUT_ALPHA_FILTERED, "wb") as f:
        f.write(base64.b64decode(r["imageData"].split(",", 1)[1]))
    img1 = load_rgba(OUT_ALPHA_FILTERED)
    region1 = ImageStat.Stat(img1.crop((450, 20, 620, 340)))
    print(f"hole + glow-only: far-interior region mean RGBA={[round(v, 2) for v in region1.mean]} "
          f"-> {OUT_ALPHA_FILTERED}")
    check("far-transparent input stays transparent after filtering", region1.mean[3] < 8,
          f"mean alpha={region1.mean[3]:.2f}, threshold <8")
    check("no colour bleeds into the transparent region", max(region1.mean[:3]) < 8,
          f"mean RGB=({region1.mean[0]:.2f}, {region1.mean[1]:.2f}, {region1.mean[2]:.2f}), "
          f"threshold <8 each")

    await c.request("RemoveSourceFilter", {"sourceName": "hole", "filterName": "fx"})
    await c.request("RemoveInput", {"inputName": "hole"})


async def main(port=None):
    """Runs the full 31-check harness once. `port` overrides the module-level default (4460) so a
    caller that already knows its sandbox's obs-websocket port -- tools/render-proof.sh via argv,
    tools/proof.py via its own sandbox setup -- can point this at it. CHECKS/FAILURES are cleared
    first so a process that calls main() more than once (tools/proof.py does not; this only guards
    against surprise) still reports an honest armed count for the run it just did."""
    global PORT, URL
    if port is not None:
        PORT = port
        URL = f"ws://127.0.0.1:{PORT}"
    CHECKS.clear()
    FAILURES.clear()
    make_tone(WAV)
    make_pack_zip(ZIP)
    make_quadrant_png(QUAD_PNG)
    make_alphahole_png(ALPHAHOLE_PNG)
    ws = None
    try:
        ws, c = await open_client("driver")
        await run(c)
        await run_filter(c)
        await run_spatial(c)
        await run_transparency(c)
    except FFTimeout as e:
        check(e.what, False, f"timed out after {REQ_TIMEOUT}s — possible deadlock")
    finally:
        if ws is not None:
            try:
                await asyncio.wait_for(ws.close(), timeout=5)
            except asyncio.TimeoutError:
                pass


def summarize():
    """Prints the same pass/armed/expected summary the old module-level code printed, and returns
    the exit code that code passed to sys.exit -- split out so a caller (tools/proof.py) can fold
    this harness's CHECKS/FAILURES into a larger report instead of the process just exiting here."""
    passed = len(CHECKS) - len(FAILURES)
    print(f"\n{passed}/{len(CHECKS)} checks passed ({len(CHECKS)} armed, {EXPECTED_CHECKS} expected)")
    if len(CHECKS) < EXPECTED_CHECKS:
        print(f"  note: only {len(CHECKS)} of {EXPECTED_CHECKS} expected checks ran -- the run was cut "
              f"short (a timeout?) before reaching the rest")
    if FAILURES:
        print(f"{len(FAILURES)} failed check(s): {FAILURES}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    _port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    asyncio.run(main(_port))
    sys.exit(summarize())
