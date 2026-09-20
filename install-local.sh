#!/usr/bin/env bash
# Installs the built plugin into an OBS user plugin dir. Default: the real one. Pass a config dir for a sandbox.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cfg="${1:-$HOME/.config/obs-studio}"
dst="$cfg/plugins/obs-foxfire"
mkdir -p "$dst/bin/64bit" "$dst/data"
cp "$here/build_x86_64/obs-foxfire.so" "$dst/bin/64bit/obs-foxfire.so"
rm -rf "$dst/data" && cp -r "$here/data" "$dst/data"
echo "installed to $dst"
