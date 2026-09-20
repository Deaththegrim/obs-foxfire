#!/usr/bin/env python3
"""First-render proof for the Foxfire Visualizer source.

Drives a sandboxed OBS over obs-websocket: builds a scene, adds a Foxfire Visualizer, screenshots
it against silence and again against a generated tone, and reports how many pixels each frame lit.
The sandbox and the OBS process are the caller's job (see tools/render-proof.sh).
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
OUT_SILENT = "/tmp/ff-first-render.png"
OUT_TONE = "/tmp/ff-first-render-tone.png"
OUT_GAP = "/tmp/ff-first-render-gap.png"
WAV = "/tmp/ff-tone.wav"
ZIP = "/tmp/ff-demo2.zip"
W, H = 640, 360


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


def count_pixels(b64, path):
    payload = b64.split(",", 1)[1]
    with open(path, "wb") as f:
        f.write(base64.b64decode(payload))
    img = Image.open(path).convert("RGBA")
    alpha = img.getchannel("A").getdata()
    nonzero = sum(1 for a in alpha if a > 0)
    lit = sum(1 for a in alpha if a > 8)
    return nonzero, lit, img.size


async def main():
    make_tone(WAV)
    make_pack_zip(ZIP)
    async with websockets.connect(f"ws://127.0.0.1:{PORT}", max_size=None) as ws:
        hello = json.loads(await ws.recv())
        assert hello["op"] == 0, hello
        await ws.send(json.dumps({"op": 1, "d": {"rpcVersion": 1}}))
        ident = json.loads(await ws.recv())
        assert ident["op"] == 2, ident
        c = Client(ws)
        print("version:", json.dumps(await c.request("GetVersion"))[:200])

        await c.request("CreateScene", {"sceneName": "ffproof"})
        await c.request("SetCurrentProgramScene", {"sceneName": "ffproof"})
        await c.request("CreateInput", {
            "sceneName": "ffproof", "inputName": "ff", "inputKind": "foxfire_visualizer",
            "inputSettings": {"pack": "demo", "preset": "bars", "width": W, "height": H,
                              "audio_mode": 0},
            "sceneItemEnabled": True})
        await asyncio.sleep(1.5)

        shot = await c.request("GetSourceScreenshot", {
            "sourceName": "ff", "imageFormat": "png", "imageWidth": W, "imageHeight": H})
        silent = count_pixels(shot["imageData"], OUT_SILENT)
        print(f"silence: non-transparent={silent[0]} lit(a>8)={silent[1]} size={silent[2]} -> {OUT_SILENT}")

        items = await c.request("GetInputPropertiesListPropertyItems",
                                {"inputName": "ff", "propertyName": "preset"})
        names = [i["itemValue"] for i in items["propertyItems"]]
        print("preset list:", names)
        assert "bars" in names, names

        await c.request("CreateInput", {
            "sceneName": "ffproof", "inputName": "tone", "inputKind": "ffmpeg_source",
            "inputSettings": {"local_file": WAV, "is_local_file": True, "looping": True},
            "sceneItemEnabled": True})
        await asyncio.sleep(1.0)
        await c.request("SetInputSettings", {
            "inputName": "ff", "inputSettings": {"audio_mode": 1, "audio_source": "tone"}})
        await asyncio.sleep(1.5)

        shot = await c.request("GetSourceScreenshot", {
            "sourceName": "ff", "imageFormat": "png", "imageWidth": W, "imageHeight": H})
        tone = count_pixels(shot["imageData"], OUT_TONE)
        print(f"tone:    non-transparent={tone[0]} lit(a>8)={tone[1]} size={tone[2]} -> {OUT_TONE}")

        # the property plumbing: "l0.gap" is the bars shader's own knob, reached only through
        # ff_instance_update -> ff_renderer_apply_settings. Widening the gap must thin the bars.
        await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"l0.gap": 0.55}})
        await asyncio.sleep(1.0)
        shot = await c.request("GetSourceScreenshot", {
            "sourceName": "ff", "imageFormat": "png", "imageWidth": W, "imageHeight": H})
        gap = count_pixels(shot["imageData"], OUT_GAP)
        print(f"gap=0.55: non-transparent={gap[0]} lit(a>8)={gap[1]} -> {OUT_GAP}")

        # the Install pack path picker: its modified callback only fires when the properties are
        # built, which obs_source_properties does via obs_properties_apply_settings -- so setting
        # the path and then asking for a property's items drives the real trigger path.
        await c.request("SetInputSettings", {"inputName": "ff", "inputSettings": {"install_zip": ZIP}})
        packs = await c.request("GetInputPropertiesListPropertyItems",
                                {"inputName": "ff", "propertyName": "pack"})
        ids = [i["itemValue"] for i in packs["propertyItems"]]
        left = (await c.request("GetInputSettings", {"inputName": "ff"}))["inputSettings"].get("install_zip")
        print(f"install: pack list now {ids}, install_zip left as {left!r}")
        assert "demo2" in ids, ids

        # destroy path: removing the input runs ff_instance_destroy with OBS still live, so a
        # deadlock or a double free shows up as a dead socket rather than a truncated shutdown log
        await c.request("RemoveInput", {"inputName": "ff"})
        await asyncio.sleep(1.0)
        await c.request("GetVersion")
        print("destroy: source removed, OBS still answering")


asyncio.run(main())
