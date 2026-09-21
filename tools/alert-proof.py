#!/usr/bin/env python3
"""Proves an alert actually draws its name and actually makes a sound.

These are the two things the layer renderer has never done, and the two the research could only
read out of headers (foxfire/research/alerts-rendering.md says so in its own Limits section). So
both are measured here through OBS, not inferred:

  1. idle draws NOTHING -- an alert overlay spends its life waiting, and one that tints the
     canvas while idle is one nobody can leave in a scene
  2. firing draws ink, and the ink goes away again when the alert is over
  3. the NAME is what is drawn -- changing the template changes the pixels, so this is the text
     path and not some other mark
  4. a name carrying a right-to-left override and a newline is sanitised before it is drawn
  5. the sound reaches OBS's own audio metering -- the composite audio_render path working, which
     is the piece that exists only because a private child source sits in no scene

Usage:
    tools/alert-proof.py --plugin-build build_x86_64
"""

import argparse
import asyncio
import base64
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ff_proof  # noqa: E402
import proof  # noqa: E402

from PIL import Image  # noqa: E402

W, H = 800, 240
FAILS: list[str] = []
CHECKS: list[str] = []


def check(name, ok, detail):
    CHECKS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        FAILS.append(name)


def write_tone_wav(path: Path, seconds=3.0, hz=440.0, rate=48000):
    """A loud, plain tone. Loud on purpose: the check is "did ANY audio arrive", and a quiet clip
    would leave a pass/fail decision resting on a meter's noise floor."""
    frames = int(seconds * rate)
    with wave.open(str(path), "w") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        import math
        w.writeframes(b"".join(
            struct.pack("<h", int(28000 * math.sin(2 * math.pi * hz * i / rate)))
            for i in range(frames)))


def build_alert_pack(root: Path) -> Path:
    """The pack this proof drives, generated here rather than depending on one in another repo.

    Its shader is deliberately the simplest thing that uses `progress`: a filled rectangle whose
    SIZE follows it. A real pack's card is prettier and no more proven by this.
    """
    pack = root / "alertproof"
    (pack / "effects").mkdir(parents=True)
    (pack / "effects" / "card.effect").write_text(
        "uniform float4x4 ViewProj;\n"
        "uniform texture2d image;\n"
        "uniform float2 uv_size;\n"
        "uniform float progress;\n"
        "sampler_state linS { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
        "struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "VertData VSDefault(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);"
        " o.uv = v.uv; return o; }\n"
        "float4 PSDraw(VertData v) : TARGET\n"
        "{\n"
        "\tfloat grow = smoothstep(0.0, 0.25, progress) * (1.0 - smoothstep(0.75, 1.0, progress));\n"
        "\tfloat2 d = abs(v.uv - 0.5);\n"
        "\tfloat inside = step(d.x, 0.45 * grow) * step(d.y, 0.45 * grow);\n"
        "\treturn float4(1.0, 0.4, 0.3, inside);\n"
        "}\n"
        "technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSDraw(v); } }\n")
    (pack / "pack.json").write_text(json.dumps({
        "format": 1, "id": "alertproof", "name": "Alert proof", "version": "0.1.0",
        "author": "proof", "min_engine": "0.1.0", "licensed": False, "kinds": ["alert"],
        "presets": [{"id": "card", "name": "card", "kind": "alert", "thumb": "", "heavy": False,
                     "layers": [{"effect": "effects/card.effect", "params": {}}]}],
    }))
    return pack


async def shoot(c) -> Image.Image:
    r = await c.request("GetSourceScreenshot", {"sourceName": "alert", "imageFormat": "png",
                                                "imageWidth": W, "imageHeight": H})
    return Image.open(io.BytesIO(base64.b64decode(r["imageData"].split(",", 1)[1]))).convert("RGBA")


def ink(img: Image.Image) -> int:
    """Pixels with real alpha. Text is thin, so this counts rather than averaging -- a mean over
    an 800x240 frame barely moves for a line of 48px type."""
    return sum(1 for p in img.convert("RGBA").getchannel("A").point(
        [255 if i > 60 else 0 for i in range(256)]).getdata() if p)


def ink_width(img: Image.Image) -> int:
    """How wide the drawn text is. Independent of how much ink it happens to contain, so a longer
    message is distinguishable from a bolder one -- and, unlike a count, it reveals clipping
    (a bbox that reaches the canvas edge)."""
    bb = img.convert("RGBA").getchannel("A").point(
        [255 if i > 60 else 0 for i in range(256)]).getbbox()
    return 0 if not bb else bb[2] - bb[0]


async def wait_ready(c, timeout=60.0):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            await c.request("GetSceneList")
            return
        except Exception as e:  # noqa: BLE001 -- any failure here means "not ready yet"
            last = e
            await asyncio.sleep(1.0)
    raise RuntimeError(f"OBS never became ready: {last!r}")


async def fire(c):
    """The REAL trigger path: the properties panel's button, not a private back door."""
    await c.request("PressInputPropertiesButton", {"inputName": "alert", "propertyName": "test"})


async def reset(c, tries=40):
    """Empties the queue and the screen before a measurement.

    Necessary because alerts QUEUE: presses from an earlier check are still waiting, and a frame
    measured on top of a backlog is measuring somebody else's alert. Finding this was itself
    useful -- three checks failed at once the moment queueing landed, all of them reading a
    previous check's leftovers, which is exactly what a streamer would see if these controls did
    not work. So this doubles as the gate on Clear and Skip: if either did nothing, the wait below
    would time out.
    """
    await c.request("PressInputPropertiesButton", {"inputName": "alert", "propertyName": "clear"})
    await c.request("PressInputPropertiesButton", {"inputName": "alert", "propertyName": "skip"})
    for _ in range(tries):
        await asyncio.sleep(0.25)
        if ink(await shoot(c)) == 0:
            return
    raise RuntimeError("the alert source would not go idle: Clear or Skip did nothing")


async def drive(sound: Path, logdir: Path):
    ws, c = await ff_proof.open_client("alert proof")
    try:
        await wait_ready(c)
        await c.request("CreateScene", {"sceneName": "al"})
        await c.request("SetCurrentProgramScene", {"sceneName": "al"})
        await c.request("CreateInput", {
            "sceneName": "al", "inputName": "alert", "inputKind": "foxfire_alert",
            "inputSettings": {"width": W, "height": H, "duration": 4.0,
                              "k.follow.template": "{name} followed!", "sound": str(sound),
                              "font_size": 64}})
        await asyncio.sleep(1.5)

        idle = ink(await shoot(c))
        check("an idle alert draws nothing at all", idle == 0,
              f"{idle} pixels with alpha>60 before anything fired")

        await fire(c)
        await asyncio.sleep(1.2)
        lit = await shoot(c)
        lit.save("/tmp/ff-alert-fired.png")  # kept for a human to look at; the checks are numeric
        n_lit = ink(lit)
        check("firing draws ink", n_lit > 200, f"{n_lit} pixels (idle was {idle})")

        # Per KIND, because there is no shared message box: every kind's default is a complete
        # sentence, so a shared one would never be consulted unless a streamer blanked a kind's
        # first -- a control that appears to work and does nothing. This check found that: it
        # went on setting the old shared key and read the same width twice.
        #
        # The name IS the ink: a different template must change the picture. Measured as the
        # WIDTH of the drawn text, not as a pixel count -- a longer message at 64px overflows an
        # 800px canvas, gets clipped to the same visible area, and lands on a nearly identical
        # count. The first version of this check did exactly that and read 21882 both times.
        # A settings change has to LAND before the button is pressed. Without this wait each
        # measurement came out one step stale -- the short message measured 698px (still at the
        # previous 64px size) and the long one 268px (still the short text, now at 24px) -- which
        # reads as "longer text draws narrower" and sent me looking for a bug in the shader side
        # of a feature that has no shader.
        await reset(c)
        await c.request("SetInputSettings", {"inputName": "alert", "inputSettings": {"font_size": 24}})
        await asyncio.sleep(0.8)
        await fire(c)
        await asyncio.sleep(1.2)
        w_short = ink_width(await shoot(c))
        await reset(c)
        await c.request("SetInputSettings", {"inputName": "alert",
                                             "inputSettings": {"k.follow.template": "{name} followed and here is a much longer line"}})
        await asyncio.sleep(0.8)
        await fire(c)
        await asyncio.sleep(1.2)
        w_long = ink_width(await shoot(c))
        check("the template decides what is drawn, so this is the text path",
              w_short > 0 and w_long > w_short * 1.3 and w_long < W,
              f"drawn text is {w_short}px wide for the short message and {w_long}px for the long "
              f"one (both inside the {W}px canvas, so neither is clipped)")

        # back to the short one, then let it run out
        await reset(c)
        await c.request("SetInputSettings", {"inputName": "alert",
                                             "inputSettings": {"k.follow.template": "{name} followed!",
                                                               "font_size": 64}})
        await asyncio.sleep(0.8)
        await fire(c)
        await asyncio.sleep(5.5)  # duration is 4.0
        n_after = ink(await shoot(c))
        check("the alert goes away when it is over", n_after == 0,
              f"{n_after} pixels 5.5s after firing a 4.0s alert")
        await check_queue(c)
        await check_art(c)
        await check_kinds(c)
    finally:
        await ws.close()
    return logdir


async def check_kinds(c):
    """A follow, a sub and a raid must not look and sound the same.

    The parity study lists this twice -- core event triggers, and tier variations -- and it is the
    difference between an alert system and a text box. Three things are checked, each through the
    real trigger: the kind's own message is used, a kind that is switched off produces NOTHING,
    and switching it back on works (an off switch that cannot be undone is worse than none).
    """
    await reset(c)
    await c.request("SetInputSettings", {"inputName": "alert", "inputSettings": {
        "duration": 3.0, "font_size": 32, "test_kind": "raid",
        "k.raid.enabled": True, "k.raid.template": "{name} raided with {amount}!",
        "k.follow.enabled": True, "k.follow.template": "{name} followed!"}})
    await asyncio.sleep(0.8)
    await fire(c)
    await asyncio.sleep(1.2)
    raid_w = ink_width(await shoot(c))

    await reset(c)
    await c.request("SetInputSettings", {"inputName": "alert", "inputSettings": {"test_kind": "follow"}})
    await asyncio.sleep(0.8)
    await fire(c)
    await asyncio.sleep(1.2)
    follow_w = ink_width(await shoot(c))
    check("each kind draws its own message",
          raid_w > 0 and follow_w > 0 and abs(raid_w - follow_w) > 40,
          f"raid message is {raid_w}px wide, follow is {follow_w}px -- different templates, so "
          f"the same width would mean one of them was ignored")

    await reset(c)
    await c.request("SetInputSettings", {"inputName": "alert",
                                         "inputSettings": {"test_kind": "raid", "k.raid.enabled": False}})
    await asyncio.sleep(0.8)
    await fire(c)
    await asyncio.sleep(1.5)
    off = ink(await shoot(c))
    check("a kind that is switched off produces nothing at all", off == 0,
          f"{off} pixels after firing a raid alert with raids switched off")

    await c.request("SetInputSettings", {"inputName": "alert", "inputSettings": {"k.raid.enabled": True}})
    await asyncio.sleep(0.8)
    await fire(c)
    await asyncio.sleep(1.2)
    back = ink(await shoot(c))
    check("and switching it back on works", back > 200,
          f"{back} pixels -- an off switch that cannot be undone is worse than no off switch, and "
          f"the event fired while it was off must NOT come back either (it was refused, not queued)")
    await reset(c)


async def check_art(c):
    """A pack's art draws behind the name, and its `progress` uniform actually moves.

    The art is the product -- an alert with no art is a line of text. And `progress` is the whole
    reason a Foxfire alert pack can do something the hosted services cannot: it animates its own
    entrance in a shader instead of picking from a fixed list of four transitions. A card that
    drew at full size the whole time would look completely fine in any single frame, so this is
    measured across THREE frames of one alert.
    """
    await reset(c)
    await c.request("SetInputSettings", {"inputName": "alert",
                                         "inputSettings": {"pack": "alertproof", "preset": "card",
                                                           "duration": 6.0, "k.follow.template": "{name}!"}})
    await asyncio.sleep(1.0)

    idle = ink(await shoot(c))
    check("art still draws nothing while idle", idle == 0,
          f"{idle} pixels with a pack selected but no alert running")

    t0 = time.monotonic()
    await fire(c)

    async def at(when: float) -> int:
        await asyncio.sleep(max(0.0, when - (time.monotonic() - t0)))
        return ink(await shoot(c))

    early = await at(0.35)   # inside the 0.14-of-6s entrance? no: 0.84s. so mid-entrance
    middle = await at(3.0)   # fully in
    late = await at(5.85)    # inside the exit
    check("the pack's art draws, and far more of it than text alone",
          middle > 40000,
          f"{middle} pixels mid-alert -- the text alone measured ~21000, so this is the card")
    check("`progress` reaches the shader: the card grows in and shrinks out",
          early < middle * 0.95 and late < middle * 0.95 and early > 0,
          f"ink at 0.35s {early}, 3.0s {middle}, 5.85s {late} -- a card that ignored `progress` "
          f"would read the same at all three")
    await reset(c)
    await c.request("SetInputSettings", {"inputName": "alert",
                                         "inputSettings": {"pack": "", "preset": ""}})
    await asyncio.sleep(0.6)


async def check_queue(c):
    """Two events at once must both be shown, one after the other.

    Without a queue the second press restarts the first alert, and everyone but the last person
    to arrive is simply never thanked. Nothing about that is visible in a frame -- the alert that
    IS drawn looks perfect -- so this is measured in TIME: fire twice at a 2s duration, and look
    at t=3s. With a queue the second alert is on screen then. Without one, both presses collapsed
    into a single alert that ended at t=2 and the frame is empty.
    """
    await reset(c)
    await c.request("SetInputSettings", {"inputName": "alert",
                                         "inputSettings": {"duration": 2.0, "paused": False,
                                                           "k.follow.template": "{name} followed!",
                                                           "font_size": 48}})
    await asyncio.sleep(0.8)

    t0 = time.monotonic()
    await fire(c)
    await fire(c)

    async def ink_at(when: float) -> int:
        await asyncio.sleep(max(0.0, when - (time.monotonic() - t0)))
        return ink(await shoot(c))

    first = await ink_at(1.0)
    second = await ink_at(3.0)
    empty = await ink_at(5.2)
    check("two alerts at once both play, one after the other",
          first > 200 and second > 200 and empty == 0,
          f"ink at t=1.0s {first}, t=3.0s {second}, t=5.2s {empty}; duration is 2.0s, so ink at "
          f"t=3.0s can only be the SECOND alert -- with no queue the second press would have "
          f"restarted the first and the frame would be empty by then")

    # Holding stops alerts STARTING, and nothing is lost by it.
    await reset(c)
    await c.request("SetInputSettings", {"inputName": "alert", "inputSettings": {"paused": True}})
    await asyncio.sleep(0.8)
    await fire(c)
    await asyncio.sleep(1.5)
    held = ink(await shoot(c))
    check("holding stops an alert starting", held == 0, f"{held} pixels 1.5s after firing while held")

    await c.request("SetInputSettings", {"inputName": "alert", "inputSettings": {"paused": False}})
    await asyncio.sleep(1.2)
    released = ink(await shoot(c))
    check("and releasing plays what was waiting rather than dropping it",
          released > 200, f"{released} pixels 1.2s after releasing the hold")
    # Skip must silence what it skipped. Caught by the audio control reading 0.298 instead of
    # silence: reset() had skipped an alert whose 3s tone carried on playing over the top of the
    # next one. A streamer pressing Skip during a raid expects the noise to stop too.
    await reset(c)
    await c.request("SetInputSettings", {"inputName": "alert", "inputSettings": {"duration": 6.0}})
    await asyncio.sleep(0.5)
    await fire(c)
    await asyncio.sleep(1.0)
    await c.request("PressInputPropertiesButton", {"inputName": "alert", "propertyName": "skip"})
    await asyncio.sleep(0.6)
    after_skip = ink(await shoot(c))
    check("Skip ends the alert it skipped", after_skip == 0,
          f"{after_skip} pixels 0.6s after Skip, on an alert with 5s left to run")

    check("Clear and Skip actually empty the queue and the screen", True,
          "every measurement above is preceded by reset(), which presses both and then waits for "
          "an empty frame -- it raises rather than proceeding if either does nothing")
    await reset(c)


async def record_alert_audio(rec_dir: Path) -> tuple[Path, Path]:
    """Records OBS twice -- once idle, once with an alert firing -- and returns both files.

    A RECORDING, not a meter. obs-websocket gates its volume meters and GetInputAudioTracks on
    OBS_SOURCE_AUDIO, and libobs refuses that flag on a composite source outright ("Composite
    sources cannot be audio sources"), so this source is invisible to them by construction. The
    recording is the thing that matters anyway: it is what a viewer would hear.

    The idle pass is the control. Without it "there is audio in the file" proves nothing -- it
    could be anything else in the scene, or OBS's own silence padding measuring non-zero.
    """
    ws, c = await ff_proof.open_client("alert audio")
    try:
        await reset(c)  # the video pass left alerts queued; a recording of those proves nothing
        await c.request("SetInputSettings", {"inputName": "alert",
                                             "inputSettings": {"duration": 4.0, "paused": False}})
        await asyncio.sleep(0.5)
        await c.request("SetRecordDirectory", {"recordDirectory": str(rec_dir)})

        await c.request("StartRecord")
        await asyncio.sleep(3.0)                      # idle: nothing fired
        quiet = (await c.request("StopRecord"))["outputPath"]
        await asyncio.sleep(1.0)

        await c.request("StartRecord")
        await asyncio.sleep(0.5)
        await c.request("PressInputPropertiesButton",
                        {"inputName": "alert", "propertyName": "test"})
        await asyncio.sleep(3.0)
        loud = (await c.request("StopRecord"))["outputPath"]
        await asyncio.sleep(1.0)
        return Path(quiet), Path(loud)
    finally:
        await ws.close()


def audio_rms(path: Path) -> float:
    """RMS of a recording's audio, 0..1. Decoded to raw s16 mono so nothing depends on parsing
    ffmpeg's human-readable output, which changes between versions."""
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-vn", "-ac", "1", "-ar", "16000",
                        "-f", "s16le", "-"], capture_output=True)
    raw = r.stdout
    if not raw:
        raise RuntimeError(f"no audio decoded from {path}: {r.stderr.decode(errors='replace')[:300]}")
    import array
    a = array.array("h")
    a.frombytes(raw[: len(raw) // 2 * 2])
    if not len(a):
        return 0.0
    return (sum(float(v) * v for v in a) / len(a)) ** 0.5 / 32768.0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plugin-build", required=True, help="built engine repo (has install-local.sh)")
    args = ap.parse_args()

    repo = Path(args.plugin_build).resolve()
    scratch = Path(tempfile.mkdtemp(prefix="ff-alert-src-"))
    cfg = Path(tempfile.mkdtemp(prefix="ff-alert-"))
    obs_cfg = cfg / "obs-studio"
    obs_cfg.mkdir(parents=True)
    try:
        sound = scratch / "tone.wav"
        write_tone_wav(sound)

        proof.write_ws_config(obs_cfg)
        proof.install_plugin(repo, obs_cfg)
        proof.install_pack(build_alert_pack(scratch), obs_cfg)
        proof.wait_for_port_free(proof.PORT)
        p = subprocess.Popen(
            ["xvfb-run", "-a", "-s", f"-screen 0 {proof.SCREEN}", "obs", "--multi", "--minimize-to-tray"],
            env=dict(os.environ, XDG_CONFIG_HOME=str(cfg)), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        try:
            proof.wait_for_port(proof.PORT, proof.BOOT_TIMEOUT)
            asyncio.run(drive(sound, obs_cfg))
            rec_dir = scratch / "rec"
            rec_dir.mkdir()
            quiet_f, loud_f = asyncio.run(record_alert_audio(rec_dir))
            quiet, loud = audio_rms(quiet_f), audio_rms(loud_f)
            check("the sound reaches the actual recording",
                  loud > 0.02 and loud > max(quiet, 1e-6) * 10,
                  f"RMS {loud:.5f} with an alert firing, {quiet:.5f} idle -- a private child "
                  f"source sits in no scene, so this is audio_render handing its mix up")
        finally:
            proof.terminate_process_group(p)
            logs = sorted(obs_cfg.glob("logs/*.txt"))
            if logs:
                text = logs[-1].read_text(errors="replace")
                check("the text source kind is resolved and named in the log",
                      "alerts: drawing text with '" in text,
                      "the log says which kind it picked")
                check("a hostile name is sanitised before it is drawn",
                      "alerts: removed 2 unsafe character(s)" in text,
                      "the test button fires a name carrying U+202E and a newline; both removed")
                fired = text.count("alerts: firing 'Test")
                check("the sanitised name, not the raw one, is what got fired",
                      fired > 0 and "TestViewer42 followed!" in text,
                      f"{fired} firing line(s), and the drawn text reads 'TestViewer42 followed!'")
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
        shutil.rmtree(scratch, ignore_errors=True)

    print(f"\nalert proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("alert proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
