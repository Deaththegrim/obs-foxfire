# Foxfire

## Introduction

Foxfire is a free GPL OBS Studio plugin: an audio-reactive layered shader visualizer with effects, by KitsuneStudio. It is built from the [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate), which includes:

* Boilerplate plugin source code
* A CMake project file
* GitHub Actions workflows and repository actions

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

## Quick Start

An absolute bare-bones [Quick Start Guide](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide) is available in the wiki.

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

## Signing and Notarizing on macOS

Basic concepts of codesigning and notarization on macOS are explained in the correspodning [Wiki article](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS) which has a specific section for the [GitHub Actions setup](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS#setting-up-code-signing-for-github-actions).
