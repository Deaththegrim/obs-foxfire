#!/usr/bin/env python3
"""First-render proof and gate for the Foxfire Visualizer source.

Drives a sandboxed OBS over obs-websocket: builds a scene, adds a Foxfire Visualizer, screenshots
it against silence and again against a generated tone, exercises the layer-parameter and pack
install paths, then hammers the properties thread while the video thread renders and updates.
Every invariant is asserted and every measured number is printed with its unit; the script exits
non-zero on the first failed check. The sandbox and the OBS process are tools/render-proof.sh's
job.
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
WAV = "/tmp/ff-tone.wav"
ZIP = "/tmp/ff-demo2.zip"
W, H = 640, 360
STRESS_ROUNDS = 30

FAILURES = []


def check(name, ok, detail):
    """Records one gate. Nothing here prints a verdict it has not measured."""
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILURES.append(name)
    return ok


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

    async def request(self, kind, data=None):
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


async def identify(ws):
    hello = json.loads(await ws.recv())
    assert hello["op"] == 0, hello
    await ws.send(json.dumps({"op": 1, "d": {"rpcVersion": 1}}))
    ident = json.loads(await ws.recv())
    assert ident["op"] == 2, ident
    return Client(ws)


class Shot:
    """One screenshot, in the three pixel units this proof distinguishes.

    any:  alpha > 0    -- anything at all was written
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


async def stress(name):
    """C1's path: the UI/RPC thread rebuilding properties (which rescans packs and walks the
    renderer's layer array) while the video thread runs update() and renders. Two connections, so
    the two request streams genuinely overlap instead of queueing behind one another."""
    async with websockets.connect(URL, max_size=None) as wa, \
               websockets.connect(URL, max_size=None) as wb:
        ca, cb = await identify(wa), await identify(wb)

        async def hammer_props():
            for _ in range(STRESS_ROUNDS):
                await ca.request("GetInputPropertiesListPropertyItems",
                                 {"inputName": name, "propertyName": "preset"})

        async def hammer_settings():
            for i in range(STRESS_ROUNDS):
                await cb.request("SetInputSettings", {
                    "inputName": name, "inputSettings": {"l0.gap": 0.2 if i % 2 else 0.55}})

        await asyncio.gather(hammer_props(), hammer_settings())


async def main():
    make_tone(WAV)
    make_pack_zip(ZIP)
    async with websockets.connect(URL, max_size=None) as ws:
        c = await identify(ws)
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
        check("silence renders nothing", silent.core == 0,
              f"core={silent.core} px, expected 0 px")

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
        check("tone lights the bars", tone.core > 5000,
              f"core={tone.core} px, threshold >5000 px")

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
        # obs_data_clear + update the button does. With no user value for "l0.gap" the renderer
        # must put the PRESET's 0.2 back, not leave the last user value in place.
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

        # The Install pack picker's modified callback only fires when the properties are built,
        # which obs_source_properties does via obs_properties_apply_settings -- so setting the
        # path and then asking for a property's items drives the real trigger path.
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
        check("OBS survives the properties/update race",
              bool(alive.get("obsVersion")),
              f"{STRESS_ROUNDS} properties rebuilds x {STRESS_ROUNDS} setting writes on 2 "
              f"connections, OBS {alive.get('obsVersion')} still answering")
        stressed = await screenshot(c, OUT_STRESS)
        print(f"after the race: {stressed}")
        check("still rendering after the race", stressed.core > 5000,
              f"core={stressed.core} px, threshold >5000 px")

        # destroy path: removing the input runs ff_instance_destroy with OBS still live, so a
        # deadlock or a double free shows up as a dead socket rather than a truncated shutdown log
        await c.request("RemoveInput", {"inputName": "ff"})
        await asyncio.sleep(1.0)
        await c.request("GetVersion")
        check("destroy leaves OBS healthy", True, "source removed, OBS still answering")


asyncio.run(main())
print(f"\n{len(FAILURES)} failed check(s)" + (f": {FAILURES}" if FAILURES else ""))
sys.exit(1 if FAILURES else 0)
