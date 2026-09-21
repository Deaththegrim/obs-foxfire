#!/usr/bin/env bash
# Installs the built Foxfire plugins into an OBS user plugin dir. Default: the real one.
# Pass a config dir for a sandbox.
#
# Each plugin is its own directory under plugins/, which is OBS's own layout and the reason a
# streamer can install one without the other. They still share a packs directory --
# plugin_config/foxfire/packs, see ff_shared_config_path in src/ff-pack.c -- so a pack installed
# through either is visible to both.
#
# FOXFIRE_PLUGINS overrides which ones to install, space separated, for a test that needs the
# alerts plugin present WITHOUT the visualizer (or the other way round). That is not a hypothetical:
# "installable on its own" is a claim, and a claim with no way to exercise it is one nobody checks.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cfg="${1:-$HOME/.config/obs-studio}"
plugins="${FOXFIRE_PLUGINS:-obs-foxfire obs-foxfire-alerts}"

for name in $plugins; do
	so="$here/build_x86_64/$name.so"
	if [ ! -f "$so" ]; then
		echo "install-local: $so is not built" >&2
		exit 1
	fi
	dst="$cfg/plugins/$name"
	mkdir -p "$dst/bin/64bit"
	cp "$so" "$dst/bin/64bit/$name.so"
	# Both plugins carry the same data/: the locale they share, and the bundled packs, which are
	# shared on purpose for the same reason the installed packs directory is.
	rm -rf "$dst/data" && cp -r "$here/data" "$dst/data"
	echo "installed $name to $dst"
done
