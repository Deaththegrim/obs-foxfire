#!/usr/bin/env python3
"""Asserts a control that appears in more than one shader is DECLARED identically in each.

OBS's effect language has no #include -- packforge's lint refuses it, because includes do not
resolve into a pack directory (measured). So the shared controls are COPIED into every shader
that wants them, and copies drift: a range widened in three files and not the fourth, a label
reworded, a default changed. "Size" then quietly means something different depending on which
preset the streamer picked, which is the kind of thing nobody reports as a bug -- they just
decide the pack is fiddly.

Compared PER CONTROL, not per block, and that distinction is the whole design:

  * a shader may legitimately omit a control. pulse.effect has no `smoothing` because it reads a
    single level, not a spectrum, and a smoothing knob there would do nothing. Demanding an
    identical block would have forced a dead control into it to satisfy a checker.
  * a shader may legitimately have its own helpers. wave.effect smooths a SIGNED waveform, which
    is not the same arithmetic as smoothing a 0..1 band.

What is never legitimate is the same named control declared two different ways.

Armed by mutation, 9 checks, every one of them measured to bite:

    widen `size`'s range in one shader only ......... FAIL 'size' is declared differently
    stop calling ff_on_shape() in wave .............. FAIL defines ff_on_shape() and never calls it
    stop calling ff_place() in radial ............... FAIL defines ff_place() and never calls it
    drop `* onShape` from bars (call kept) .......... FAIL calls ff_on_shape() into 'onShape' ...
    drop `* onShape` from wave (call kept) .......... FAIL   ... and never reads 'onShape'
    flip ff_place()'s rotation sign in wave only .... FAIL ff_place() has two implementations
    delete the whole Placement block from bars ...... FAIL calls ff_place() but declares none
    delete only `rotate_deg` from bars .............. FAIL has a PARTIAL Placement block
    point it at a directory with no .effect files ... exit 2, NOTHING INSPECTED

The last one is not a formality. A checker that silently passes on nothing inspected is worse than
no checker, because it reports a clean result; this one reports exit 2 both when it finds no
shaders and when it finds shaders carrying none of the shared controls.

Usage:
    tools/check-placement.py <pack dir> [<pack dir> ...]
"""

import re
import sys
from pathlib import Path

# uniform <type> <name> <annotations...> = default;   -- the whole declaration, normalised
UNIFORM = re.compile(
    r"^\s*uniform\s+(\w+)\s+(\w+)\s*(<[^>]*>)?\s*(=\s*[^;]+)?;", re.M)

# Controls the pack shares. A name not listed here is a shader's own business.
SHARED = {"pos_x", "pos_y", "size", "rotate_deg", "opacity", "gain", "smoothing", "punch"}

# Shared helpers. Defined in a shader but never called there = the control it implements is dead
# in that preset, and nothing else notices: the pack still compiles and still renders, so a render
# proof passes. Measured -- wave.effect carried an ff_band with a `sampler_state` parameter, a
# signature that does NOT compile when called, through a full 16/16 placement proof, because
# nothing called it.
# The Placement block is GENERATED as a unit, so a shader carrying part of it is always a defect
# -- either the block was half-edited, or most of it was deleted. This is the check that would
# have caught the regression that started all of this: five of six packs shipped with the whole
# block missing, and a checker that only compares declarations WHERE THEY APPEAR said nothing,
# because omission is legitimate (pulse.effect has no `smoothing`). Per-control comparison cannot
# see a deletion; per-shader completeness can.
PLACEMENT_SET = ("pos_x", "pos_y", "size", "rotate_deg", "opacity")
COMMENT = re.compile(r"/\*.*?\*/", re.S)

HELPERS = ("ff_place", "ff_on_shape", "ff_band")
DEFINES = re.compile(r"^\s*(?:float\d?|float[234]?)\s+(ff_\w+)\s*\(", re.M)

# `float onShape = ff_on_shape(uv);` and then nothing ever reads onShape. The helper IS called, so
# the uncalled check above passes, and the shader compiles and renders -- with the control silently
# doing nothing, which is the exact defect this file exists to catch. Found by mutating: the first
# version of this checker let that one through.
ASSIGNED = re.compile(r"^\s*(?:float[234]?)\s+(\w+)\s*=\s*(ff_\w+)\s*\(", re.M)

# The helper's BODY, comments and whitespace removed. Identical controls in front of two different
# implementations is the drift that actually costs something: "Smoothing 0.5" meaning one blend
# here and another there is invisible in the panel and invisible in a render proof.
BODY = re.compile(r"^\s*(?:float[234]?)\s+(ff_\w+)\s*\([^)]*\)\s*\n\{(.*?)\n\}", re.M | re.S)


def norm(s: str) -> str:
    return re.sub(r"\s+", " ", s or "").strip()


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    seen: dict[str, list[tuple[Path, str]]] = {}
    bodies: dict[str, list[tuple[Path, str]]] = {}
    unused: list[str] = []
    helpers_checked = 0
    scanned = 0
    for root in sys.argv[1:]:
        for eff in sorted(Path(root).rglob("*.effect")):
            scanned += 1
            text = eff.read_text()
            code = COMMENT.sub("", text)
            for m in DEFINES.finditer(text):
                fn = m.group(1)
                if fn not in HELPERS:
                    continue
                helpers_checked += 1
                # calls, not the definition: every occurrence of "<fn>(" minus the one that is
                # the definition itself.
                if code.count(fn + "(") - 1 < 1:
                    unused.append(f"{eff}: defines {fn}() and never calls it")
            for m in BODY.finditer(text):
                fn = m.group(1)
                if fn not in HELPERS:
                    continue
                bodies.setdefault(fn, []).append(
                    (eff, re.sub(r"\s+", " ", COMMENT.sub("", m.group(2))).strip()))
            for m in ASSIGNED.finditer(code):
                var, fn = m.group(1), m.group(2)
                if fn not in HELPERS:
                    continue
                helpers_checked += 1
                if len(re.findall(rf"\b{re.escape(var)}\b", code)) < 2:
                    unused.append(f"{eff}: calls {fn}() into '{var}' and never reads '{var}'")
            here = {m.group(2) for m in UNIFORM.finditer(text)}
            have = [c for c in PLACEMENT_SET if c in here]
            # `code` above has comments stripped. A shader that declines the block says so in a
            # comment -- "there is no ff_place() here" -- and matching that text reported a
            # correct file as broken. A guard that refuses good input costs more than one that
            # misses.
            uses_place = ("ff_place(" in code)
            if have and len(have) != len(PLACEMENT_SET):
                unused.append(f"{eff}: has a PARTIAL Placement block -- "
                              f"{', '.join(have)} but not "
                              f"{', '.join(c for c in PLACEMENT_SET if c not in here)}")
            elif uses_place and not have:
                unused.append(f"{eff}: calls ff_place() but declares none of the Placement "
                              f"controls, so nothing can drive it")
            for m in UNIFORM.finditer(text):
                name = m.group(2)
                if name not in SHARED:
                    continue
                decl = f"{m.group(1)} {norm(m.group(3))} {norm(m.group(4))}"
                seen.setdefault(name, []).append((eff, decl))

    print(f"scanned {scanned} effect file(s)")
    if scanned == 0:
        print("check-placement: NOTHING INSPECTED")
        return 2
    if not seen:
        print("check-placement: none of the shared controls appear anywhere -- NOTHING COMPARED")
        return 2

    failures = []
    for name in sorted(seen):
        found = seen[name]
        files = ", ".join(f.name for f, _ in found)
        ref_file, ref = found[0]
        bad = [(f, d) for f, d in found[1:] if d != ref]
        print(f"  {name}: {len(found)} declaration(s) in {files}"
              + ("" if not bad else "   <-- DIFFER"))
        for f, d in bad:
            failures.append(f"'{name}' is declared differently:\n"
                            f"    {ref_file}\n      {ref}\n"
                            f"    {f}\n      {d}")

    for fn in sorted(bodies):
        found = bodies[fn]
        ref_file, ref = found[0]
        for f, b in found[1:]:
            if b != ref:
                failures.append(f"{fn}() has two different implementations:\n"
                                f"    {ref_file}\n      {ref}\n"
                                f"    {f}\n      {b}")
    print(f"  compared {sum(len(v) for v in bodies.values())} helper body/bodies "
          f"of {len(bodies)} helper(s)")
    # Said out loud, because "0 of 4 shaders carry Placement" is a clean pass under the
    # per-control rules and is exactly the state this is here to prevent going unnoticed.
    withp = len(seen.get("pos_x", []))
    print(f"  {withp} of {scanned} shader(s) carry the Placement block")

    compared = sum(len(v) for v in seen.values())
    print(f"compared {compared} declaration(s) of {len(seen)} shared control(s), "
          f"{helpers_checked} shared helper definition(s)")
    for u in unused:
        failures.append(u)
    if failures:
        for f in failures:
            print("FAIL " + f)
        return 1
    print("check-placement: every shared control is declared identically everywhere")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
