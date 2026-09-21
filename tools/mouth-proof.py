#!/usr/bin/env python3
"""Proves the whole mouth path: audio in, the right drawn shape on screen.

Every layer below this is already tested on its own -- the classifier against synthesised vowels,
the band layout, the hold timing. None of that shows that the number reaches the shader or that
the shader picks the cell the number names. A mouth that is always shape A looks perfectly
plausible in a screenshot and passes every "does it render" check ever written.

So this generates a strip of its own -- one flat hue per cell -- binds it over the pack's art,
plays vowels synthesised at published formants and reads the hue back off the frame. The hue says
which cell was drawn, which is the one thing no other test here can see.

The strip is generated rather than read from the pack because the pack's art is a PLACEHOLDER
waiting to be replaced by a drawing, and in a drawing the hue is the same in every cell -- it is
one mouth in six positions. Keyed to the placeholder, this whole file would have failed the day
the pack became real. Verified by doing it: with the pack's art swapped for a drawn-style strip
in one skin tone, where hue identifies nothing, this still scores 9/9.

It also checks the Mouth controls in the properties panel. Those are ENGINE state, not shader
uniforms, so no render can show whether the panel is wired to the engine or to nothing -- the
mouth looks equally correct either way. Each is checked by making it change a shape already
known from the sweep above.

WHAT THIS DOES NOT COVER, of the five controls: "Silence threshold", "Minimum shape time" and
"Jaw bias" are here. "Closed-mouth gap" chooses between rest and a closure, and the placeholder strip
folds rest onto the closed cell, so both land on the same hue and no colour can tell them apart.
"Mouth close speed" moves `mouth_open`, which changes no cell index. Nor is the `uses_mouth()`
half covered -- obs-websocket cannot enumerate a source's properties, so whether the group
APPEARS is unproven here; what is proven is that the settings reach the classifier.

ARMED by mutation -- every number below was measured by running it, not predicted:

    control (everything wired up)            13/13 passed
    stretch ignores mouth_open                11/13
    stretch scales about its centre           12/13
    stretch scales width as well as height    12/13  -- survived until a width check existed;
                                                        it shortens and pivots correctly while
                                                        pulling the corners in, which on screen
                                                        is a mouth receding, not closing

 and, measured earlier at ten checks:

    control                                  10/10 passed
    mouth stuck on the first cell             3/10  -- the survivors are "silence draws A", the
                                                        gate check (both want A anyway) and the
                                                        pixel count: exactly the false pass this
                                                        file is for
    rest folds to the wrong shape             8/10
    mouth settings never reach the engine     7/10
    hold wired to the release field           9/10
    frication branch removed                  9/10  -- the hiss goes back to drawing an open jaw
    uses_mouth() forced false                10/10  -- UNARMED, as stated above
    pack art swapped for drawn-style art      9/9   -- the point: it must NOT fail (measured
                                                        before the jaw check existed)

    tools/mouth-proof.py --plugin-build . --pack ../foxfire/packs/mouth
"""

import argparse
import asyncio
import base64
import colorsys
import io
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ff_proof  # noqa: E402
import proof  # noqa: E402

from PIL import Image  # noqa: E402

W, H = 640, 360
SR = 48000
FAILS: list[str] = []
CHECKS: list[str] = []

# The probe strip this file draws for itself: one flat hue per cell, so a screenshot says WHICH
# cell was selected. It is generated here and bound over the pack's own art through the
# user-image path, deliberately.
#
# Reading the hues off the PACK's art is what this used to do, and it made the gate die exactly
# when the pack became real: the placeholder is a stand-in for junkie's drawing, and the first
# thing that happens to it is being replaced. A proof that cannot survive its subject being
# finished is not a proof of the subject.
CELL_HUE = {"A": 0, "B": 36, "C": 64, "D": 120, "E": 203, "F": 272,
            "G": 160, "H": 240, "X": 310}
STRIP_CELL = 256


def check(name, ok, detail):
    CHECKS.append(name)
    if not ok:
        FAILS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def render_vowel(f1: float, f2: float, f3: float, secs: float = 6.0, f0: float = 120.0):
    """A vowel: glottal pulses through three formant resonators, then normalised.

    The same source-filter model tests/test_viseme.c uses, for the same reason -- the right answer
    is known by construction. Normalising is not optional: cascaded resonators have gain that
    depends on the formants, so without it each vowel arrives at a different level and clipped.
    """
    n = int(SR * secs)
    y1 = [0.0, 0.0, 0.0]
    y2 = [0.0, 0.0, 0.0]
    a1, a2 = [], []
    for hz, bw in ((f1, 80.0), (f2, 100.0), (f3, 150.0)):
        r = math.exp(-math.pi * bw / SR)
        a1.append(2.0 * r * math.cos(2.0 * math.pi * hz / SR))
        a2.append(-r * r)
    out = [0.0] * n
    phase, step = 0.0, f0 / SR
    for i in range(n):
        phase += step
        x = 0.0
        if phase >= 1.0:
            phase -= 1.0
            x = 1.0
        for k in range(3):
            y = x + a1[k] * y1[k] + a2[k] * y2[k]
            y2[k] = y1[k]
            y1[k] = y
            x = y
        out[i] = x
    mx = max(abs(v) for v in out) or 1.0
    return [v / mx * 0.3 for v in out]


def make_probe_strip(path: Path, cells: int = 6):
    """One flat, saturated cell per shape, in strip order.

    `cells` is not decoration. At six, the only fold the engine can reach is X->A, because it
    never returns G or H -- so six cells cannot show whether the fallback for a MISSING real
    shape works. Four can. And nine gives X a hue of its own, which is the only way to tell rest
    from a closure, since at six they are the same cell.
    """
    order = ["A", "B", "C", "D", "E", "F", "G", "H", "X"][:cells]
    img = Image.new("RGBA", (STRIP_CELL * len(order), STRIP_CELL), (0, 0, 0, 0))
    for i, name in enumerate(order):
        r, g, b = colorsys.hsv_to_rgb(CELL_HUE[name] / 360.0, 0.85, 0.95)
        cell = Image.new("RGBA", (STRIP_CELL, STRIP_CELL),
                         (int(r * 255), int(g * 255), int(b * 255), 255))
        img.paste(cell, (i * STRIP_CELL, 0))
    img.save(path)


def render_fricative(hz: float, bw: float, secs: float = 6.0):
    """Noise through one broad resonator, then lip radiation -- an "sss".

    The radiation term is the same one the vowel gets. Applying it to one and not the other
    would BE the difference the classifier is being asked to find.
    """
    n = int(SR * secs)
    r = math.exp(-math.pi * bw / SR)
    a1 = 2.0 * r * math.cos(2.0 * math.pi * hz / SR)
    a2 = -r * r
    y1 = y2 = prev = 0.0
    seed = 1
    out = [0.0] * n
    for i in range(n):
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        x = (seed >> 8) / 8388608.0 - 1.0
        y = x + a1 * y1 + a2 * y2
        y2, y1 = y1, y
        out[i] = y - prev
        prev = y
    mx = max(abs(v) for v in out) or 1.0
    return [v / mx * 0.3 for v in out]


def write_wav(path: Path, samples):
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(v * 32000)) for v in samples))


async def shoot(c, name):
    r = await c.request("GetSourceScreenshot", {"sourceName": name, "imageFormat": "png",
                                                "imageWidth": W, "imageHeight": H})
    return Image.open(io.BytesIO(base64.b64decode(r["imageData"].split(",", 1)[1]))).convert("RGBA")


def dominant_hue(img: Image.Image):
    """Hue of the drawn pixels, ignoring anything transparent or near-black.

    The probe strip is flat colour, so the filtering is not for its benefit -- it is what lets
    the same reading work on drawn art, where the opening of the mouth is near-black and the
    margins are transparent. Without it those average every shape toward the same dark grey.
    """
    hues, weight = [], 0.0
    for r, g, b, a in img.getdata():
        if a < 200:
            continue
        mx, mn = max(r, g, b), min(r, g, b)
        if mx < 80 or mx - mn < 40:   # too dark or too grey to carry a hue
            continue
        h, _, _ = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        hues.append(h * 360.0)
        weight += 1.0
    if not hues:
        return None, 0
    # circular mean, because hue wraps and A sits at 0
    x = sum(math.cos(math.radians(h)) for h in hues)
    y = sum(math.sin(math.radians(h)) for h in hues)
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0, int(weight)


def drawn_box(img: Image.Image):
    """Bounding box of the drawn pixels, and its height."""
    box = img.getchannel("A").point(lambda a: 255 if a > 100 else 0).getbbox()
    return box, (box[3] - box[1]) if box else 0


def nearest_shape(hue):
    if hue is None:
        return None
    best, bd = None, 1e9
    for name, h in CELL_HUE.items():
        d = abs((hue - h + 180) % 360 - 180)
        if d < bd:
            best, bd = name, d
    return best


async def drive(pack_id: str, wavs: dict, strip: Path, strip4: Path, strip9: Path):
    ws, c = await ff_proof.open_client("mouth proof")
    seen = {}
    try:
        for _ in range(60):
            try:
                await c.request("GetVersion")
                break
            except Exception:
                await asyncio.sleep(1)
        await c.request("CreateScene", {"sceneName": "mo"})
        await c.request("SetCurrentProgramScene", {"sceneName": "mo"})
        await c.request("CreateInput", {
            "sceneName": "mo", "inputName": "voice", "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": str(wavs["ee"]),
                              "looping": True}})
        await asyncio.sleep(1.5)
        await c.request("CreateInput", {
            "sceneName": "mo", "inputName": "mouth", "inputKind": "foxfire_visualizer",
            "inputSettings": {"pack": pack_id, "preset": "mouth-strip", "width": W, "height": H,
                              "audio_mode": 1, "audio_source": "voice",
                              # the probe strip, over the pack's own art. "l0.mouth" is the
                              # renderer's key for layer 0's user-supplied image.
                              "l0.mouth": str(strip)}})
        await asyncio.sleep(2.0)

        # Silence first: nothing playing, so the mouth must be at rest -- which folds to A.
        await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
            "local_file": str(wavs["silence"])}})
        await c.request("TriggerMediaInputAction", {
            "inputName": "voice", "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
        await asyncio.sleep(3.0)
        hue, px = dominant_hue(await shoot(c, "mouth"))
        seen["silence"] = (nearest_shape(hue), hue, px)

        for vowel in ("ee", "ah", "oo", "aw", "hiss"):
            await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
                "local_file": str(wavs[vowel])}})
            await c.request("TriggerMediaInputAction", {
                "inputName": "voice", "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
            await asyncio.sleep(3.0)
            hue, px = dominant_hue(await shoot(c, "mouth"))
            seen[vowel] = (nearest_shape(hue), hue, px)

        async def set_mouth(key, value):
            await c.request("SetInputSettings",
                            {"inputName": "mouth", "inputSettings": {key: value}})

        async def rest_then(path, settle=3.0):
            """Silence long enough to reach rest, then this file from the top.

            The silence is not padding. Coming out of rest is deliberately immediate, so this
            is what makes the first voiced frame choose a shape rather than inherit whatever
            the previous clip left up.
            """
            for f in (wavs["silence"], path):
                await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
                    "local_file": str(f)}})
                await c.request("TriggerMediaInputAction", {
                    "inputName": "voice",
                    "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
                await asyncio.sleep(2.5 if f is wavs["silence"] else settle)
            return dominant_hue(await shoot(c, "mouth"))

        # GATE, at the top of its own slider range: the test tone sits near 0.08, so a mouth
        # that reads the setting stops answering the audio at all and falls back to rest.
        await set_mouth("mouth.gate", 0.3)
        hue, px = await rest_then(wavs["ah"])
        seen["gated"] = (nearest_shape(hue), hue, px)
        await set_mouth("mouth.gate", 0.04)

        # HOLD: one file, "ee" running straight into "oo". At the shipped hold the mouth has
        # moved to F by a second into the second vowel; held for eight seconds it is still on
        # B, the shape the FIRST vowel chose. Same audio both times -- only the setting differs.
        hue, px = await rest_then(wavs["ee_oo"], settle=4.5)
        seen["ee_oo_free"] = (nearest_shape(hue), hue, px)
        await set_mouth("mouth.hold_ms", 8000.0)
        hue, px = await rest_then(wavs["ee_oo"], settle=4.5)
        seen["ee_oo_held"] = (nearest_shape(hue), hue, px)
        await set_mouth("mouth.hold_ms", 80.0)

        # JAW BIAS, the fifth control: "oo" is a pucker at the shipped thresholds, and trimming
        # the jaw axis wide open turns the same audio into the wide-open shape. Two cells apart
        # in the strip and 150 degrees apart in hue, so nothing subtle is being read here.
        await set_mouth("mouth.jaw_bias", 0.15)
        hue, px = await rest_then(wavs["oo"])
        seen["oo_biased"] = (nearest_shape(hue), hue, px)
        await set_mouth("mouth.jaw_bias", 0.0)

        # ---- the other preset: ONE image stretched, which reads mouth_open and not viseme ----
        #
        # Nothing else in this file or the test suite touches that builtin. It is fed every
        # frame, documented as the way to drive a single-mouth rig, and until this existed no
        # shader anywhere read it -- a slot built for somebody else to fill and never filled.
        # TWO requests, and the order matters: changing preset erases every "l<n>.*" key, because
        # those belong to whichever preset was loaded when they were written. Sent together, the
        # size below is wiped by the preset change in the same update and silently does nothing
        # -- which is how the first version of this check ran at full size without noticing.
        await c.request("SetInputSettings", {"inputName": "mouth", "inputSettings": {
            "preset": "mouth-stretch"}})
        await asyncio.sleep(1.0)
        # Half size, so the art does not touch the top of the canvas. At full size it does, the
        # top edge reads 0 whatever the mouth is doing, and "the top edge barely moves" would be
        # satisfied by the frame clipping it rather than by the shader pivoting there.
        await c.request("SetInputSettings", {"inputName": "mouth", "inputSettings": {
            "l0.size": 0.5}})
        await asyncio.sleep(1.5)
        for name, wav in (("loud", wavs["ah"]), ("quiet", wavs["silence"])):
            await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
                "local_file": str(wav)}})
            await c.request("TriggerMediaInputAction", {
                "inputName": "voice",
                "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
            await asyncio.sleep(3.0)
            box, h = drawn_box(await shoot(c, "mouth"))
            seen[f"stretch_{name}"] = (None, box, h)
        await c.request("SetInputSettings", {"inputName": "mouth", "inputSettings": {
            "preset": "mouth-strip"}})
        await asyncio.sleep(1.0)
        await c.request("SetInputSettings", {"inputName": "mouth", "inputSettings": {
            "l0.mouth": str(strip)}})
        await asyncio.sleep(1.5)

        # ---- the fold, on a strip that does NOT have every shape ----
        #
        # Four cells: A B C D. "aw" asks for E, which is not there, and E is a MID-OPEN mouth
        # -- so it must land on C. The old ladder sent every missing real shape to B, which put
        # a mid-open vowel on the teeth-together cell; clamping instead would put it on the
        # wide-open D. This vowel is the one that distinguishes all three answers, which is why
        # it is here and "oo" is not: a missing F lands on B under the old rule and the new one
        # alike, so it proves nothing.
        #
        # With six cells none of this is reachable at all: the engine never returns G or H, so
        # X->A is the only fold a six-cell strip can exercise.
        await c.request("SetInputSettings", {"inputName": "mouth", "inputSettings": {
            "l0.mouth": str(strip4), "l0.shapes": 4.0}})
        await asyncio.sleep(1.5)
        hue, px = await rest_then(wavs["aw"])
        seen["fold_aw_4cell"] = (nearest_shape(hue), hue, px)

        # ---- rest is not a closure, on a strip where they are different cells ----
        #
        # Nine cells gives X a hue of its own. At six, rest folds onto the closed A and the two
        # are indistinguishable by colour, which is why "Closed-mouth gap" was the one control
        # this file said it could not cover.
        #
        # No timing race: rather than catching a short gap, the gap is made long and the control
        # is made longer. Three seconds of silence is rest at the shipped 200 ms and is still a
        # closure at 5000 ms. Ignored, both read X.
        await c.request("SetInputSettings", {"inputName": "mouth", "inputSettings": {
            "l0.mouth": str(strip9), "l0.shapes": 9.0}})
        await asyncio.sleep(1.5)
        for name, closure in (("default", 200.0), ("long", 5000.0)):
            await set_mouth("mouth.closure_ms", closure)
            # speak first, then stop: a mouth that has never heard speech rests rather than
            # closing, so without this both readings would be X whatever the setting says
            await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
                "local_file": str(wavs["ah"])}})
            await c.request("TriggerMediaInputAction", {
                "inputName": "voice",
                "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
            await asyncio.sleep(2.0)
            await c.request("SetInputSettings", {"inputName": "voice", "inputSettings": {
                "local_file": str(wavs["silence"])}})
            await c.request("TriggerMediaInputAction", {
                "inputName": "voice",
                "mediaAction": "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART"})
            await asyncio.sleep(3.0)
            hue, px = dominant_hue(await shoot(c, "mouth"))
            seen[f"gap_{name}"] = (nearest_shape(hue), hue, px)
        await set_mouth("mouth.closure_ms", 200.0)
        await c.request("SetInputSettings", {"inputName": "mouth", "inputSettings": {
            "l0.mouth": str(strip), "l0.shapes": 6.0}})
        await asyncio.sleep(1.5)

        # And the pack's OWN art, once, because nothing else here looks at it any more. Not by
        # colour -- that is the coupling this file just got rid of -- but it has to draw
        # something: "" means the pack's own image, and a strip that failed to load draws
        # nothing at all.
        await c.request("SetInputSettings",
                        {"inputName": "mouth", "inputSettings": {"l0.mouth": ""}})
        await rest_then(wavs["ah"])
        _, px = dominant_hue(await shoot(c, "mouth"))
        seen["pack_art"] = (None, None, px)
    finally:
        await ws.close()
    return seen


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True)
    ap.add_argument("--pack", required=True)
    args = ap.parse_args()

    repo = Path(args.plugin_build).resolve()
    pack_dir = Path(args.pack).resolve()
    pack_id = json.loads((pack_dir / "pack.json").read_text())["id"]

    cfg = Path(tempfile.mkdtemp(prefix="ff-mouth-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    scratch = Path(tempfile.mkdtemp(prefix="ff-mouth-src-"))
    try:
        wavs = {}
        # the same formants tests/test_viseme.c uses, and the shape each one should draw
        for name, (f1, f2, f3) in {"ee": (240, 2400, 2900), "ah": (850, 1610, 2600),
                                   "oo": (250, 595, 2400), "aw": (360, 640, 2400)}.items():
            wavs[name] = scratch / f"{name}.wav"
            write_wav(wavs[name], render_vowel(f1, f2, f3))

        # A hiss. Nothing about it is a vowel, and before the classifier looked for frication
        # it drew C -- a half-open jaw on every "s" in every sentence.
        wavs["hiss"] = scratch / "hiss.wav"
        write_wav(wavs["hiss"], render_fricative(5200.0, 3000.0))

        # "ee" running straight into "oo" with NO gap between them. The gap is the point: a
        # silence long enough to reach the mouth resets it to rest, and coming out of rest is
        # deliberately immediate, which bypasses the hold entirely. Switching files to change
        # vowel would therefore pass whether the hold works or not.
        wavs["ee_oo"] = scratch / "ee_oo.wav"
        write_wav(wavs["ee_oo"],
                  render_vowel(240, 2400, 2900, secs=3.0) + render_vowel(250, 595, 2400, secs=3.0))
        strip = scratch / "probe-strip.png"
        make_probe_strip(strip)
        strip4 = scratch / "probe-strip-4.png"
        make_probe_strip(strip4, cells=4)
        strip9 = scratch / "probe-strip-9.png"
        make_probe_strip(strip9, cells=9)
        wavs["silence"] = scratch / "silence.wav"
        with wave.open(str(wavs["silence"]), "w") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
            w.writeframes(b"\0\0" * (SR * 6))

        proof.write_ws_config(obs_cfg)
        proof.install_plugin(repo, obs_cfg)
        proof.install_pack(pack_dir, obs_cfg)
        proof.wait_for_port_free(proof.PORT)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi",
             "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            seen = asyncio.run(drive(pack_id, wavs, strip, strip4, strip9))
        finally:
            proof.terminate_process_group(p)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    want = {"silence": "A", "ee": "B", "ah": "D", "oo": "F", "hiss": "B", "aw": "E"}
    for key, expect in want.items():
        got, hue, px = seen.get(key, (None, None, 0))
        check(f"{key} draws shape {expect}", got == expect,
              f"hue {hue if hue is None else round(hue)} over {px} lit pixels -> {got}")

    # And they are not all the same cell, which a mouth stuck on one shape would also satisfy
    # check by check above if the expectations happened to agree.
    # Only the cells picked by hue belong here. pack_art and the two stretch readings carry no
    # shape at all, and counting their None as a distinct value would let a mouth stuck on one
    # cell satisfy the very check written to catch that.
    skip = ("pack_art", "stretch_loud", "stretch_quiet", "fold_aw_4cell",
            "gap_default", "gap_long")
    shapes = {k: v[0] for k, v in seen.items() if k not in skip}
    check("the shape actually changes with the audio", len(set(shapes.values())) >= 3,
          f"{shapes} -- a mouth stuck on one shape renders perfectly well and is the failure "
          f"this whole file exists to catch")

    # The two Mouth controls whose effect is visible in WHICH CELL is drawn. Nothing else can
    # see these: they are engine state, not shader uniforms, so every render looks correct
    # whether the panel is wired to the engine or to nothing.
    got, hue, px = seen.get("gated", (None, None, 0))
    check("raising the silence threshold silences the mouth", got == "A",
          f"a tone at ~0.08 under a gate of 0.3 -> {got} (hue {hue if hue is None else round(hue)}); "
          f"D would mean the setting never reached the engine")
    check("the pack's own art draws", seen.get("pack_art", (None, None, 0))[2] > 500,
          f"{seen.get('pack_art', (None, None, 0))[2]} lit pixels with the shipped strip bound "
          f"-- every other check here uses a strip this file generates, so this is the only one "
          f"that touches the art the pack ships")
    lb, lh = seen.get("stretch_loud", (None, None, 0))[1:]
    qb, qh = seen.get("stretch_quiet", (None, None, 0))[1:]
    check("the stretched mouth follows the audio", lh > qh * 1.3 and qh > 0,
          f"drawn height {lh}px on a vowel against {qh}px in silence -- a shader that ignores "
          f"mouth_open draws the same height either way, and renders perfectly well doing it")
    # and it stretches DOWNWARD: the upper lip is where a real one is, just under the nose, and
    # a mouth that grows about its middle climbs into the face every time it shuts
    check("it opens downward, not about its centre",
          bool(lb) and bool(qb) and abs(lb[1] - qb[1]) <= 3 and lb[3] > qb[3] + 3,
          f"top edge {lb[1] if lb else '?'} -> {qb[1] if qb else '?'} (should barely move), "
          f"bottom edge {lb[3] if lb else '?'} -> {qb[3] if qb else '?'} (should rise a lot)")
    # and only downward. A uniform scale passed both checks above -- it shortens the mouth
    # correctly and pivots correctly -- while also pulling the corners in, which on screen is a
    # mouth receding into the face rather than closing.
    lw = (lb[2] - lb[0]) if lb else 0
    qw = (qb[2] - qb[0]) if qb else 0
    check("it does not narrow as it closes", lw > 0 and abs(lw - qw) <= max(3, lw * 0.04),
          f"drawn width {lw}px on a vowel against {qw}px in silence -- a jaw closing does not "
          f"pull the corners of the mouth in")

    got, hue, px = seen.get("fold_aw_4cell", (None, None, 0))
    check("a missing shape folds by aperture, not by index", got == "C",
          f"\"aw\" asks for E on a four-cell strip that stops at D -> {got} "
          f"(hue {hue if hue is None else round(hue)}); B is where the old index ladder sent "
          f"every missing shape, D is where clamping would send it")

    gd = seen.get("gap_default", (None, None, 0))[0]
    gl = seen.get("gap_long", (None, None, 0))[0]
    check("the closed-mouth gap decides rest from closure", gd == "X" and gl == "A",
          f"three seconds of silence after speech: shipped 200 ms -> {gd} (rest), 5000 ms -> "
          f"{gl} (still a closure); want X then A, and two X's would mean the setting never "
          f"reached the engine")

    got, hue, px = seen.get("oo_biased", (None, None, 0))
    check("the jaw bias re-opens the mouth", got == "D",
          f"the same \"oo\" that draws F untrimmed draws {got} at a bias of +0.15 "
          f"(hue {hue if hue is None else round(hue)}); F would mean the setting never arrived")
    free = seen.get("ee_oo_free", (None, None, 0))[0]
    held = seen.get("ee_oo_held", (None, None, 0))[0]
    check("the hold time keeps a shape up", free == "F" and held == "B",
          f"same audio, ee running into oo: shipped hold -> {free}, hold 8000 ms -> {held} "
          f"(want F then B)")

    print(f"\nmouth proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("mouth proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
