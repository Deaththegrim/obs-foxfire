#!/usr/bin/env python3
"""Copies the shared Placement/Response block from the reference shader into other pack shaders.

OBS's effect language has no #include, so every shader that wants these controls carries its own
copy. Typing the copies by hand is how they drift, and tools/check-placement.py exists because
they did. This generates them instead, from ONE reference file, and is idempotent -- run it again
after editing the reference and every target picks up the change.

It only inserts the DECLARATIONS. Wiring the shader's own maths through ff_place()/ff_band() is a
per-shader job and is left to a human: a placement block a shader declares and never uses is dead
controls, which is the exact defect this whole thing is fixing, so check-placement.py fails on it.

    tools/apply-placement.py --reference <ref.effect> --blocks place,shape,response <target.effect>

Blocks:
    place     the Placement uniforms and ff_place()   -- every shader that draws a shape
    shape     ff_on_shape()                           -- shapes that would otherwise tile when shrunk
    response  the Response uniforms and ff_band()     -- shaders that read the spectrum in bands
"""

import argparse
import sys
from pathlib import Path

# Each block starts at its own header comment. It ENDS at whichever comes first of the other
# blocks' headers and the shader's PSDraw -- not at one fixed successor, because a shader may
# carry any subset: card.effect has `place` alone, and looking for a `shape` header that is not
# there is how the first version of this crashed instead of stripping it.
MARKS = {
    "place": "/* --- Placement: the same five controls",
    "shape": "/* 1 inside the shape's own 0..1 box",
    "response": "/* --- Response:",
}
END = "float4 PSDraw"


def bounds(text: str, name: str):
    """(start, end) of `name`'s block in `text`, or None when it is not there."""
    head = MARKS[name]
    if head not in text:
        return None
    i = text.index(head)
    ends = [text.index(m, i + len(head)) for m in list(MARKS.values()) + [END]
            if m != head and m in text[i + len(head):]]
    if not ends:
        sys.exit(f"apply-placement: {name!r} block has no end marker after it")
    return i, min(ends)


def slice_of(ref: str, name: str) -> str:
    b = bounds(ref, name)
    if b is None:
        sys.exit(f"apply-placement: the reference has no {name!r} block "
                 f"(looked for {MARKS[name]!r})")
    return ref[b[0]:b[1]]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reference", required=True)
    ap.add_argument("--blocks", required=True, help="comma-separated: place,shape,response")
    ap.add_argument("targets", nargs="+")
    a = ap.parse_args()

    ref = Path(a.reference).read_text()
    names = [b.strip() for b in a.blocks.split(",") if b.strip()]
    for n in names:
        if n not in MARKS:
            sys.exit(f"apply-placement: unknown block {n!r}; known: {', '.join(MARKS)}")
    block = "".join(slice_of(ref, n) for n in names)

    for t in a.targets:
        path = Path(t)
        s = path.read_text()
        if END not in s:
            sys.exit(f"apply-placement: {path} has no {END!r} to insert before")
        # Idempotent: strip EVERY known block before inserting the requested ones, so this is a
        # SYNC, not an append. Two reasons it strips all of them and not just the ones being
        # written: a second run would otherwise double every uniform and libobs refuses the whole
        # effect, and re-running with a NARROWER set would leave the dropped block behind as
        # orphaned dead controls -- which is what happened to card.effect, caught by
        # check-placement.py's uncalled-helper rule rather than by anything here.
        for n in MARKS:
            b = bounds(s, n)
            if b is not None:
                s = s[:b[0]] + s[b[1]:]
        i = s.index(END)
        path.write_text(s[:i] + block + s[i:])
        print(f"applied [{', '.join(names)}] to {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
