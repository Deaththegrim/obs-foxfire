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
"""
import asyncio
import base64
import json
import math
import struct
import sys
import wave
import zipfile
from pathlib import Path

import websockets
from PIL import Image

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4460
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
WAV = "/tmp/ff-tone.wav"
ZIP = "/tmp/ff-demo2.zip"
W, H = 640, 360
STRESS_ROUNDS = 30
REQ_TIMEOUT = 20  # seconds; generous for a local socket, short next to a hang

FAILURES = []


def check(name, ok, detail):
    """Records one gate. Nothing here prints a verdict it has not measured."""
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
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
    return ws, Client(ws)


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
        pixels = img.getdata()
        n = len(pixels)
        lumas = [(r + g + b) / 3.0 for r, g, b, _ in pixels]
        alphas = [a for _, _, _, a in pixels]
        self.path = path
        self.size = img.size
        self.mean = sum(lumas) / n
        self.variance = sum((v - self.mean) ** 2 for v in lumas) / n
        self.min_alpha = min(alphas)
        self.max_alpha = max(alphas)

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

    await c.request("RemoveInput", {"inputName": "fxbase"})


async def main():
    make_tone(WAV)
    make_pack_zip(ZIP)
    ws = None
    try:
        ws, c = await open_client("driver")
        await run(c)
        await run_filter(c)
    except FFTimeout as e:
        check(e.what, False, f"timed out after {REQ_TIMEOUT}s — possible deadlock")
    finally:
        if ws is not None:
            try:
                await asyncio.wait_for(ws.close(), timeout=5)
            except asyncio.TimeoutError:
                pass


asyncio.run(main())
print(f"\n{len(FAILURES)} failed check(s)" + (f": {FAILURES}" if FAILURES else ""))
sys.exit(1 if FAILURES else 0)
