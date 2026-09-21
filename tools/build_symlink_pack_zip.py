#!/usr/bin/env python3
"""Builds the reviewer's Critical-1 reproduction zip: a pack whose zip contains a symlink entry
pointing OUTSIDE the packs tree.

This is the exact shape the whole-branch review used to arm the defect at src/ff-pack.c:
  - one top-level folder named like a pack id (passes the "exactly one top-level folder" check)
  - a symlink entry inside it, pointing at an arbitrary directory outside packs/ (passes the
    ".." / prefix checks, because the symlink's NAME is perfectly in-tree -- only its TARGET,
    which check_zip_entries never looks at, escapes)
  - a pack.json with a selectable min_engine (default "0.0.0" so the pack WOULD pass manifest
    validation, exposing the symlink defence; pass "99.0.0" to test the cleanup path where
    load_pack_dir() fails early and remove_recursive() must be called on the half-extracted temp).

Usage:
    build_symlink_pack_zip.py <out.zip> <symlink-target-dir> [pack_id] [min_engine]

The entries use the same S_IFLNK-in-external_attr encoding Info-ZIP's `zip -y` produces, which is
what `unzip`/`os_safe_replace`-style extraction on a real buyer's machine actually restores as a
real symlink -- not a stand-in or a mock.
"""
import stat
import sys
import zipfile


def _entry(name, mode):
    zi = zipfile.ZipInfo(name)
    zi.external_attr = mode << 16
    if name.endswith("/"):
        zi.external_attr |= 0x10  # MS-DOS directory flag, so `unzip -Z1`/short listings agree it's a dir
    return zi


def build(out_path, link_target, pack_id="ember", min_engine="0.0.0"):
    manifest = (
        '{ "format": 1, "id": "%s", "name": "Reproduction pack", "version": "1.0.0", '
        '"min_engine": "%s", "licensed": false, "presets": [] }' % (pack_id, min_engine)
    )
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(_entry(f"{pack_id}/", stat.S_IFDIR | 0o755), "")
        z.writestr(_entry(f"{pack_id}/pack.json", stat.S_IFREG | 0o644), manifest)
        z.writestr(_entry(f"{pack_id}/link", stat.S_IFLNK | 0o777), link_target)
    return out_path


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4, 5):
        sys.exit(__doc__)
    pack_id = sys.argv[3] if len(sys.argv) >= 4 else "ember"
    min_engine = sys.argv[4] if len(sys.argv) >= 5 else "0.0.0"
    out = build(sys.argv[1], sys.argv[2], pack_id, min_engine)
    print(f"wrote {out}: top-level folder + a symlink entry -> {sys.argv[2]!r} + min_engine={min_engine}")
