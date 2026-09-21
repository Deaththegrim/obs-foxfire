#!/usr/bin/env python3
"""Drives tests/calibrate_cli.c against WAVs written here, with known right answers.

WHY THIS IS NOT A ctest OF ITS OWN: calibrate_cli needs a recording and this repo has none, and
a test that skips when its input is missing passes having inspected nothing. So the input is
BUILT -- a synthesised vowel whose shape is known by construction, wrapped in the file shapes the
parser was specifically written for.

The one that matters is the LIST chunk. load_wav walks RIFF chunks instead of assuming a 44-byte
header precisely because anything that writes metadata -- ffmpeg included -- puts one before the
samples; read from byte 44 that metadata is analysed AS AUDIO and the tool reports confidently on
noise. Nothing proved that path until this file existed.

The rest are refusals. Every one of them was a print-and-continue that left the exit code at 0,
so a run over nine unreadable files looked exactly like a clean one.

    tools/calibrate-proof.py build_x86_64/tests/calibrate_cli
"""

import math
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SR = 48000
FAILS: list[str] = []
CHECKS: list[str] = []


def check(name, ok, detail):
    CHECKS.append(name)
    if not ok:
        FAILS.append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def vowel(secs=2.0, f0=110.0, formants=((850, 80), (1610, 100), (2600, 150))):
    """"ae" -- the one vowel that classifies as D at every breathiness, so the shape mix has a
    known answer rather than a plausible one."""
    n = int(SR * secs)
    state = [[0.0, 0.0] for _ in formants]
    coef = []
    for hz, bw in formants:
        r = math.exp(-math.pi * bw / SR)
        coef.append((2.0 * r * math.cos(2.0 * math.pi * hz / SR), -r * r))
    out = [0.0] * n
    phase, step = 0.0, f0 / SR
    for i in range(n):
        phase += step
        x = 0.0
        if phase >= 1.0:
            phase -= 1.0
            x = 1.0
        for k, (a1, a2) in enumerate(coef):
            y = x + a1 * state[k][0] + a2 * state[k][1]
            state[k][1] = state[k][0]
            state[k][0] = y
            x = y
        out[i] = x
    mx = max(abs(v) for v in out) or 1.0
    return [v / mx * 0.3 for v in out]


def write_wav(path, samples, bits=16, fmt=1, list_chunk=False, empty_data=False, rate=SR):
    body = b"WAVE"
    if list_chunk:
        # what ffmpeg writes when it records who made the file
        # ODD length on purpose: RIFF chunks are word-aligned, so an odd body is followed by a
        # pad byte. Skip the padding and every subsequent chunk header is read one byte out.
        # With an even body the alignment code is never exercised, which is how a mutation
        # removing it scored a clean pass here.
        info = b"INFOISFT" + struct.pack("<I", 15) + b"Lavf60.16.100a\0"
        # the declared size is the body's, and an ODD body is followed by a pad byte that is
        # not counted in it. Writing the size without the pad is a malformed file, not a test
        # of the reader -- which is what the first version of this did, and the reader was
        # right to land one byte out.
        body += b"LIST" + struct.pack("<I", len(info)) + info + (b"\0" if len(info) % 2 else b"")
    byte_rate = rate * bits // 8
    body += b"fmt " + struct.pack("<IHHIIHH", 16, fmt, 1, rate, byte_rate, bits // 8, bits)
    if empty_data:
        data = b""
    elif bits == 16:
        data = b"".join(struct.pack("<h", int(v * 32000)) for v in samples)
    else:
        data = b"".join(struct.pack("<f", v) for v in samples)
    body += b"data" + struct.pack("<I", len(data)) + data
    Path(path).write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def run(exe, *args):
    r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def main() -> int:
    exe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pcm = vowel()
        write_wav(tmp / "plain.wav", pcm)
        write_wav(tmp / "meta.wav", pcm, list_chunk=True)
        write_wav(tmp / "float.wav", pcm, bits=32, fmt=3)
        write_wav(tmp / "eightbit.wav", pcm, bits=8)
        write_wav(tmp / "nodata.wav", [], empty_data=True)
        write_wav(tmp / "silence.wav", [0.0] * (SR * 2))

        rc, out, _ = run(exe, tmp / "plain.wav")
        # 2 s at 48 kHz over a 512-sample hop, minus the frames the analysis needs to fill its
        # window: the point is that the DENOMINATOR is printed and is the right order, not that
        # it is a magic number.
        check("a plain WAV is read and reported", rc == 0 and "voiced frames of" in out,
              f"exit {rc}; {out.splitlines()[2] if len(out.splitlines()) > 2 else out!r}")
        check("the vowel classifies as the shape it was built to be", "D=" in out and
              float(out.split("D=")[1].split("%")[0]) > 50.0,
              f"synthesised at F1 850 / F2 1610, which is D: "
              f"{out.split('shape mix:')[1].split('(')[0].strip() if 'shape mix:' in out else '?'}")

        rc2, out2, _ = run(exe, tmp / "meta.wav")
        # THE one. A LIST chunk before the samples, read as audio, is noise -- and noise reports
        # a perfectly plausible distribution.
        mix = lambda o: o.split("shape mix:")[1].split("(")[0].strip() if "shape mix:" in o else "?"
        check("a LIST chunk before the samples changes nothing",
              rc2 == 0 and mix(out2) == mix(out),
              f"same file with ffmpeg-style metadata: {mix(out2)}")

        for name, why in (("float.wav", "32-bit float"), ("eightbit.wav", "8-bit"),
                          ("nodata.wav", "an empty data chunk")):
            rc3, out3, err3 = run(exe, tmp / name)
            # the message matters as much as the exit code: "it failed" without naming the
            # file or the reason sends someone looking at the wrong input
            named = name in err3 and err3.strip() != ""
            check(f"{why} is refused, by name", rc3 != 0 and named and "voiced frames" not in out3,
                  f"exit {rc3}, stderr {err3.strip().splitlines()[0] if err3.strip() else '(NOTHING SAID)'}")

        rc4, out4, _ = run(exe, tmp / "silence.wav")
        check("silence says it is too quiet rather than reporting on nothing",
              rc4 != 0, f"exit {rc4}")

        rc5, out5, _ = run(exe, tmp / "plain.wav", tmp / "float.wav")
        check("a skipped file is named on STDOUT and changes the exit code",
              rc5 != 0 and "SKIPPED" in out5,
              f"exit {rc5}; the diagnostics go to stderr, so redirecting stdout to a report "
              f"used to hide this entirely")

        rc6, _, _ = run(exe, tmp / "plain.wav", "--jaw-bias", "0.1")
        rc7, _, _ = run(exe, "--jaw-bias", "nope", tmp / "plain.wav")
        rc8, _, _ = run(exe)
        check("the flag is taken anywhere, and a bad value is refused",
              rc6 == 0 and rc7 == 2 and rc8 == 2,
              f"trailing flag {rc6}, unparseable {rc7}, no arguments {rc8}")

    print(f"\ncalibrate proof: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} passed")
    if not CHECKS:
        print("calibrate proof: NOTHING INSPECTED")
        return 2
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main())
