# Foxfire

## Introduction

Foxfire is a free GPL OBS Studio plugin: an audio-reactive layered shader visualizer with effects, by KitsuneStudio. It is built from the [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate), which includes:

* Boilerplate plugin source code
* A CMake project file
* GitHub Actions workflows and repository actions

## What it is

Foxfire adds two OBS object types:

* **Foxfire Visualizer** (a source) — an audio-reactive layered shader visualizer. Add it to a
  scene, pick a pack, pick a preset, and it reacts to whatever audio you point it at (the master
  mix, or a specific source).
* **Foxfire Effects** (a filter) — the same layered shader engine, attached to any other source, so
  its output gets the reactive treatment instead of drawing on its own.

Both are driven by **packs**: directories of `.effect` shaders plus a `pack.json` manifest
declaring presets (named combinations of layers with default parameter values). The plugin ships
one free demo pack (`data/packs/demo`, two presets: Bars, Glow). Everything a pack can declare, and
exactly what the engine checks before it'll load one, is in
[`docs/PACK-FORMAT.md`](docs/PACK-FORMAT.md).

## Install

### Windows

The Windows installer package installs Foxfire to
`C:\ProgramData\obs-studio\plugins\obs-foxfire\`. Download the release zip, extract, run the
installer (or copy the extracted tree into that folder yourself), then start OBS.

### Linux

Two ways to install:

* **`.deb` package** — download it from a release, `sudo apt install ./obs-foxfire-<version>.deb`
  (or `sudo dpkg -i` + `sudo apt-get install -f` to pull in any missing dependency).
* **`install-local.sh`** — build from source (see Supported Build Environments below), then run
  `./install-local.sh` from the repo root. With no argument it installs into your real OBS user
  config (`~/.config/obs-studio/plugins/obs-foxfire`); pass a directory to install into a sandbox
  config instead (this is what `tools/render-proof.sh` and `tools/proof.py` do to test a build
  without touching your real OBS setup).

### macOS

Not yet released; the template's build system supports it (see Supported Build Environments) but no
release artefact has shipped. Build from source if you need it.

## Quick start

1. Open OBS, add a source, pick **Foxfire Visualizer**.
2. In its properties, set Pack to **Foxfire Demo**, Preset to **Demo Bars** (the dropdowns show each
   pack/preset's display name, not its internal id — "Demo Bars" is also the only preset offered
   here, since the demo pack's other preset, "Demo Glow", is a filter-kind preset and only shows up
   under the Effects filter below).
3. Point Audio at whatever you want it to react to (Master mix by default) and it should light up
   with whatever's playing.

For the Effects filter: add any source (e.g. a colour source, a webcam, a game capture), open its
filters, add **Foxfire Effects**, pick a pack and preset the same way (**Demo Glow** is what shows
up there for the demo pack).

## Supported Build Environments

| Platform  | Tool   |
|-----------|--------|
| Windows   | Visual Studio 17 2022 |
| macOS     | XCode 16.0 |
| Windows, macOS  | CMake 3.30.5 |
| Ubuntu 24.04 | CMake 3.28.3 |
| Ubuntu 24.04 | `ninja-build` |
| Ubuntu 24.04 | `pkg-config`
| Ubuntu 24.04 | `build-essential` |

## About licences and copying

Foxfire is GPL and its source is right here, so nothing in it can stop a rebuilt copy from loading
any pack. Paid packs carry a licence file signed by KitsuneStudio that the engine checks offline (no
network, ever); it exists to show who a pack was sold to and to expire quietly if a licence is not
renewed. If you rebuild Foxfire without that check, you can; the packs are still licensed to the
person who bought them.

## Documentation

All documentation can be found in the [Plugin Template Wiki](https://github.com/obsproject/obs-plugintemplate/wiki).

Suggested reading to get up and running:

* [Getting started](https://github.com/obsproject/obs-plugintemplate/wiki/Getting-Started)
* [Build system requirements](https://github.com/obsproject/obs-plugintemplate/wiki/Build-System-Requirements)
* [Build system options](https://github.com/obsproject/obs-plugintemplate/wiki/CMake-Build-System-Options)

## Render proof

`tools/ff_proof.py` is a headless, armed proof of the Foxfire Visualizer source and Effects filter:
it boots OBS on Xvfb in a throwaway config, installs the built plugin, and drives it over
obs-websocket through silence/tone/gap/Restore Defaults/pack-install, a properties-vs-render stress
race, source/filter destroy, and dedicated spatial (orientation + blur extent) and transparency
(alpha convention) checks -- 29 named checks, printed as an armed/expected count so a run that gets
cut short (e.g. a timeout) is visible as such, not silently reported green.

A local run needs three Python packages CI already installs as system packages (see
`.github/workflows/proof.yaml`): `websockets`, `Pillow` (`PIL`), and `numpy`. On Debian/Ubuntu:

```
sudo apt-get install python3-websockets python3-pil python3-numpy
```

(or `pip install websockets pillow numpy` in a virtualenv.)

Run it locally with `tools/render-proof.sh`, which also owns the sandbox lifecycle (fresh config
dir, `xvfb-run`, port polling, and teardown that survives a wedged destroy path):

```
tools/render-proof.sh
```

`tools/proof.py` is the packforge-facing entry point -- a thin CLI that boots its own sandbox, runs
the same `ff_proof.py` harness unchanged, and (with `--pack`) additionally installs a pack and
screenshots every preset it declares, gating each on non-blank render:

```
python3 tools/proof.py --plugin-build . --pack data/packs/demo --out build_x86_64/proof
```

With `--pack`, it also generates a throwaway copy of that pack with its id rewritten and installs
it too, to prove the install path itself (not just the bundled copy `install-local.sh` ships)
actually gets used -- see `install_probe_pack` in the report below.

Writes `<out>/report.json` (`obs`, `checks_armed`, `presets_inspected`, `presets`,
`install_probe_pack`, `warnings`, `checks`), one `<out>/<pack>-<preset>.png` per inspected preset,
and `<out>/obs.log`. Exit 0 all good; 1 any check failed, any preset blank, a `pack.json` with no
presets or an out-of-range preset count, an armed/expected check-count mismatch, or any
`[obs-foxfire]` warn:/error: line in the log (one exact, licence-public-key-is-zero warning is
allowed while `FF_PUBLIC_KEY` is unset -- see `ALLOWED_WARNING_TEXT` in `tools/proof.py`); 2 nothing
was armed (OBS never came up). Add `--keep` to keep the temp sandbox and print its path.

## GitHub Actions & CI

Default GitHub Actions workflows are available for the following repository actions:

* `push`: Run for commits or tags pushed to `master` or `main` branches.
* `pr-pull`: Run when a Pull Request has been pushed or synchronized.
* `dispatch`: Run when triggered by the workflow dispatch in GitHub's user interface.
* `build-project`: Builds the actual project and is triggered by other workflows.
* `check-format`: Checks CMake and plugin source code formatting and is triggered by other workflows.
* `proof`: Builds on Ubuntu, runs the unit tests, then runs `tools/proof.py` against the demo pack
  under software GL (`LIBGL_ALWAYS_SOFTWARE=1`) and uploads `proof-out/` (screenshots, `report.json`,
  `obs.log`) as a build artifact on every run, pass or fail.

The workflows make use of GitHub repository actions (contained in `.github/actions`) and build scripts (contained in `.github/scripts`) which are not needed for local development, but might need to be adjusted if additional/different steps are required to build the plugin.

### Retrieving build artifacts

Successful builds on GitHub Actions will produce build artifacts that can be downloaded for testing. These artifacts are commonly simple archives and will not contain package installers or installation programs.

### Building a Release

To create a release, an appropriately named tag needs to be pushed to the `main`/`master` branch using semantic versioning (e.g., `12.3.4`, `23.4.5-beta2`). A draft release will be created on the associated repository with generated installer packages or installation programs attached as release artifacts.

Don't tag from memory: follow [`docs/RELEASE.md`](docs/RELEASE.md), which covers what has to be
green first, the Windows smoke test the draft release waits on, and the public-key precondition for
any release that ships a paid pack.

## Signing and Notarizing on macOS

Basic concepts of codesigning and notarization on macOS are explained in the correspodning [Wiki article](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS) which has a specific section for the [GitHub Actions setup](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS#setting-up-code-signing-for-github-actions).
