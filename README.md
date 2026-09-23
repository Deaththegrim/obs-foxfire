# Foxfire

## Introduction

Foxfire is a free GPL OBS Studio plugin: an audio-reactive layered shader visualizer with effects, by KitsuneStudio. It is built from the [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate), which includes:

* Boilerplate plugin source code
* A CMake project file
* GitHub Actions workflows and repository actions

## What it is

Foxfire ships as **two separate plugins**, so you can install one without the other:

`obs-foxfire` — the visualizer, adding three OBS object types:

* **Foxfire Visualizer** (a source) — an audio-reactive layered shader visualizer. Add it to a
  scene, pick a pack, pick a preset, and it reacts to whatever audio you point it at (the master
  mix, or a specific source).
* **Foxfire Effects** (a filter) — the same layered shader engine, attached to any other source, so
  its output gets the reactive treatment instead of drawing on its own.
* **Foxfire Transition** (a scene transition) — the same engine again, given the outgoing and
  incoming scenes as `tex_a` and `tex_b` and where it is between them. Add it from the **+** on
  OBS's Scene Transitions box and pick a pack preset of kind `transition`; the bundled `demo` pack
  ships `dissolve`. The audio is crossfaded at equal power, so a cut does not dip in the middle.
* **Foxfire** (a dock) — a panel inside OBS listing every Foxfire object in the current scene
  collection, each with its live meters, what audio is feeding it, and pack/preset dropdowns you
  can switch from without opening Properties. Open it from **View → Docks → Foxfire**. Every
  Foxfire object is listed, scene transitions included and whether or not a transition is the one
  currently selected, so a preset can be retuned without switching to it first — the panel scrolls,
  because a collection with a pack's worth of transitions runs to thirty-odd rows.
  Built only when the plugin is configured with `ENABLE_QT` and `ENABLE_FRONTEND_API`; the
  per-source meter tap it reads is plain libobs and is in every build, so anything holding an
  `obs_source_t *` — an obs-websocket script, another plugin — can read the same values. A row on
  the master mix whose audio stops arriving says so, rather than sitting at a flat meter that looks
  exactly like a quiet stream. (Only the master mix: a source-mode row follows something that is
  entitled to fall silent — a media source that finished, a capture whose app closed — so silence
  there is not a fault.)

`obs-foxfire-alerts` — the alerts engine:

* **Foxfire Alert** (a source) — draws stream alerts (follows, subs, gift subs, resubs, bits,
  raids, channel point redemptions) using a pack's art, with per-kind messages, art and sounds,
  and a queue so a raid's alerts play in order instead of overwriting each other. It connects to
  Twitch itself over EventSub — no browser source, no hosted service, no account with anyone but
  Twitch. See [Connecting to Twitch](#connecting-to-twitch).

Both plugins share one packs directory, so a pack installed through either is visible to both.

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

There is no tagged release yet. CI builds Windows on every push to `master` and publishes
`obs-foxfire-0.1.0-windows-x64.zip` as a run artifact, carrying both plugins —
`obs-foxfire\bin\64bit\obs-foxfire.dll` and `obs-foxfire-alerts\bin\64bit\obs-foxfire-alerts.dll`
— in the layout above. Either directory can be copied on its own; they share the packs folder.

**It has never been run on Windows.** It compiles, links and packages there as of
2026-09-22, and that is all that is known: every render, alert and mouth proof in this repo
is Linux-only and has never executed against a Windows build.

### Linux

Two ways to install:

* **`.deb` package** — download it from a release, `sudo apt install ./obs-foxfire-<version>.deb`
  (or `sudo dpkg -i` + `sudo apt-get install -f` to pull in any missing dependency).
* **`install-local.sh`** — build from source (see Supported Build Environments below), then run
  `./install-local.sh` from the repo root. With no argument it installs BOTH plugins into your
  real OBS user config (`~/.config/obs-studio/plugins/obs-foxfire` and `…/obs-foxfire-alerts`);
  pass a directory to install into a sandbox config instead (this is what `tools/render-proof.sh`
  and `tools/proof.py` do to test a build without touching your real OBS setup). Set
  `FOXFIRE_PLUGINS` to install just one, e.g.
  `FOXFIRE_PLUGINS=obs-foxfire-alerts ./install-local.sh`.

### macOS

Not yet released; the template's build system supports it (see Supported Build Environments) but no
release artefact has shipped. Build from source if you need it.

## Quick start

1. Open OBS, add a source, pick **Foxfire Visualizer**.
2. In its properties, set Pack to **Foxfire Demo**, Preset to **Demo Bars** (the dropdowns show each
   pack/preset's display name, not its internal id — "Demo Bars" is also the only preset offered
   here, since the demo pack's other preset, "Demo Glow (filter)", is a filter-kind preset and only shows up
   under the Effects filter below).
3. Point Audio at whatever you want it to react to (Master mix by default) and it should light up
   with whatever's playing.

For the Effects filter: add any source (e.g. a colour source, a webcam, a game capture), open its
filters, add **Foxfire Effects**, pick a pack and preset the same way (**Demo Glow (filter)** is
what shows up there for the demo pack).

## Lipsync: a mouth on a character

Foxfire can drive a mouth from whatever it is listening to — your mic, a music track, a TTS
voice — and draw it over a character. It is a visualizer preset like any other, so it moves,
scales, rotates and fades with the Placement controls rather than needing its own source.

It works on the **shape of the sound, not the loudness**. Loudness alone is what makes a mouth
flap: every syllable looks identical. Vowel identity lives in the first two formants — how far
the jaw is open and how far forward the tongue sits — and those two things map straight onto the
classic mouth-shape set.

**The art.** One image, the shapes side by side, left to right, same cell size, the mouth in the
same place in every cell, transparent background:

| | Shape | Sounds |
| --- | --- | --- |
| 1 | **A** closed | P, B, M |
| 2 | **B** slightly open, teeth together | "EE", K, S, T, and every hiss |
| 3 | **C** open | "EH" |
| 4 | **D** wide open | "AA" as in *father* |
| 5 | **E** slightly rounded | "AO", "ER" |
| 6 | **F** puckered | "OO", "OW", W |

The pack ships two example strips and a template you can draw over, all generated by
`tools/make-mouth-strip.py` in the packs repo. Whatever you draw or generate, run
`tools/check-mouth-strip.py` over it: it measures whether every cell sits on the same anchor,
which is the one defect that no amount of redrawing the shapes fixes and that a contact sheet
will not show you. It has a `--fix` that aligns them.

Generating the art rather than drawing it works, and a video model suits it better than six
separate image generations — temporal coherence is what it is for. Render from an **open** mouth
(a shut one has no interior, so the model invents teeth differently every time), in two clips:
open→closed covers A–D, neutral→pucker covers E and F. Then `tools/pick-mouth-frames.py` measures
every frame and picks the six, and the registration check aligns them. Details in
`docs/PACK-FORMAT.md`.

**Six cells is the whole set — do not draw more.** Preston Blair and Rhubarb also define G (teeth
on the lip, for F and V) and H (tongue up, for a long L), and Foxfire will never ask for either.
Both are articulatory facts rather than spectral ones: /f/ differs from /s/ mainly by being flat
and about 15–20 dB quieter, which does not survive a microphone whose gain is a knob on the desk,
and /l/ is marked by a notch, which nothing here looks for. Rhubarb gets them by running a
phoneme recogniser over a finished file offline; doing that live would mean shipping an acoustic
model and spending the CPU on a machine that is already encoding video. A strip drawn for Rhubarb
still works — the extra cells are simply never selected.

**If you have not drawn six shapes yet**, use the *Mouth — one image, stretched* preset instead.
It takes a single drawing of an open mouth and opens and closes it with the audio. It cannot tell
an "oo" from an "ee" — nothing that only knows how far the jaw is down can — but it is the rig
most people already have. Note that only **Mouth close speed** and **Silence threshold** affect
it: the other three decide which *shape* to draw, and this preset never asks for a shape.

**Tuning it.** A preset that draws a mouth grows a **Mouth** group in the properties panel:

- **Silence threshold** — below this the mouth stops answering. Breath, a fan and a keyboard all
  have energy. Raise it if the mouth twitches when you are not talking; lower it if you are quiet.
- **Closed-mouth gap** — a gap shorter than this is a stop consonant (the closure in P, B, M) and
  the mouth shuts; longer and the speaker has stopped, so it rests.
- **Minimum shape time** — how long a shape stays up. The number that decides whether this reads
  as speech or as flapping. Shorten it for fast talkers.
- **Mouth close speed** — how fast the mouth relaxes. Opening is always immediate; a mouth that
  lags the start of a word looks dubbed.
- **Jaw bias** — raise it if the mouth stays too shut, lower it if it hangs open. This is the one
  most likely to need moving, and it is not a matter of taste: measured across four recorded
  voices, the feature it trims varies by more than the distance between the two thresholds it is
  compared against, and one of the four sat wide open on 56% of frames until it was trimmed.
  Three of the four needed nothing.

These describe your voice and your microphone rather than the art, so they survive a change of
pack or preset.

**Finding the right bias without guessing:** `calibrate_cli --jaw-bias N voice.wav` prints what
share of frames each shape gets. Run it at 0, and if one shape is taking most of them, re-run
with a bias until it is not. A worked example, on a real recording that needed one:

```
jaw bias +0.00   B=57.4%  C= 7.5%  D= 6.1%  E=10.6%  F=18.4%
jaw bias +0.03   B=51.3%  C=11.7%  D=12.8%  E= 9.5%  F=14.8%   <- even, and matches the reference
jaw bias +0.06   B=44.3%  C=15.0%  D=20.1%  E=10.0%  F=10.6%
```

What you are looking for is nothing near 0% and nothing over about 40%. A shape at 0% never
appears on that voice; a shape over 40% is a mouth mostly stuck on one thing. See below.

## Connecting to Twitch

The alerts source can draw alerts without Twitch at all — the **Test** button fires one, which is
how you set up your look. To receive real events you need a Twitch application of your own.

**Why yours and not ours:** this plugin is GPL, so it ships its source. Any client ID or secret
baked into it would be public the moment it was released. Foxfire uses the **Device Code Grant**,
which needs no secret, but it still needs an application to identify itself as — and that
application should be yours.

1. Go to <https://dev.twitch.tv/console/apps> and **Register Your Application**.
   * *Name*: anything (it is shown to you when you authorise it).
   * *OAuth Redirect URLs*: `http://localhost` — the device flow does not use it, but the form
     requires one.
   * *Category*: Broadcasting Suite. *Client Type*: **Public** (this is the part that matters —
     a confidential client would need a secret).
2. Copy the **Client ID**. There is no secret to copy; that is the point.
3. In OBS, open the Foxfire Alert source's properties, find the **Twitch** section, and paste it
   into **Twitch application client ID**.
4. Press **Connect to Twitch…**. The status line will show a code and a URL — go to
   <https://www.twitch.tv/activate>, enter the code, and approve the permissions.
5. Tick **Connect to Twitch and show real alerts**.

The status line then tells you what is happening, including how many of the seven alert types were
accepted. If it says fewer than seven, one of the permissions was not granted — each alert type
needs its own:

| Alert | Twitch permission |
| --- | --- |
| Follow | `moderator:read:followers` |
| Sub, gift sub, resub | `channel:read:subscriptions` |
| Bits | `bits:read` |
| Channel point redemption | `channel:read:redemptions` |
| **Raid** | **none** |

Raids need no permission at all, which makes them the easiest thing to test with.

**What is stored, and where.** Only a refresh token, in Foxfire's own config directory
(`plugin_config/foxfire/twitch.json`), with owner-only permissions. The access token is never
written to disk — it lasts about four hours and is fetched again as needed. Nothing is stored in
your scene collection, which is a file people share and back up. **Sign out** deletes it.

**Off by default.** The source never connects until you tick the box, because adding a source to a
scene should not start talking to your Twitch account.

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
| Ubuntu 24.04 | `libcurl4-openssl-dev` |

`libcurl` carries the alerts plugin's connection to Twitch. Windows and macOS get it from
obs-deps, which ships curl already; on Linux it is the one package the template's list does not
already name, so a source build needs `libcurl4-openssl-dev` installed. It is a hard requirement —
CMake stops with an error rather than quietly building a plugin that cannot connect.

## About licences and copying

Foxfire is GPL and its source is right here, so nothing in it can stop a rebuilt copy from loading
any pack. Paid packs carry a licence file signed by KitsuneStudio that the engine checks offline —
no network, ever, and no clock.

**A pack you bought never stops working.** There is no expiry, no grace period and nothing that can
lapse on you mid-stream. The licence carries the date your subscription last covered
(`entitled_through`); a pack carries the date that version was published (`released`); the pack
opens when `released <= entitled_through`. So everything that existed while you were subscribed is
yours permanently, offline, on a machine whose clock is wrong — and packs published after you stop
are the only thing you lose access to.

If you rebuild Foxfire without the check, you can. The packs are still licensed to the person who
bought them, and the point of the check was never to stop you: it is to show who a pack was sold to
and to decide which packs a subscription entitles someone to.

Free packs (`"licensed": false`) need none of this. The one that ships with the engine, and
`basics` in the packforge repo, are free — their effect files are CC0, so nothing about this
engine's GPL reaches your channel.

## Documentation

All documentation can be found in the [Plugin Template Wiki](https://github.com/obsproject/obs-plugintemplate/wiki).

Suggested reading to get up and running:

* [Getting started](https://github.com/obsproject/obs-plugintemplate/wiki/Getting-Started)
* [Build system requirements](https://github.com/obsproject/obs-plugintemplate/wiki/Build-System-Requirements)
* [Build system options](https://github.com/obsproject/obs-plugintemplate/wiki/CMake-Build-System-Options)

## Tuning the mouth to a voice

`calibrate_cli` measures what the classifier makes of a real recording — the percentiles of each
feature with the live thresholds marked next to them, the share of frames each shape gets, and
the frame count beside all of it. Build it with the tests and point it at 16-bit PCM WAVs of the
voice that will drive the mouth.

It is not part of `ctest`, because there is no recording in this repository and a test that skips
when its input is missing passes having inspected nothing. What to look for is written at the top
of `tests/calibrate_cli.c`: a shape stuck at 0.0% is a shape that will never appear on that
voice, and that has happened before — an early "wide open" threshold sat above where 99% of real
voiced frames reach, so the wide mouth never once fired while classifying every synthesised vowel
perfectly.

**One threshold is set against synthesised speech only** — the frication one, and its own
comment says so. The vowel thresholds were balanced against recorded speech, and although the
recording that originally did it is gone, four corpora measured since (107,259 voiced frames,
four speakers) agree with it. What is still missing is a real streaming microphone in the room
the mouth will run in, which is what the noise gate wants — hence this tool being in the tree
rather than in somebody's scratch directory.

## Render proof

`tools/ff_proof.py` is a headless, armed proof of the Foxfire Visualizer source and Effects filter:
it boots OBS on Xvfb in a throwaway config, installs the built plugin, and drives it over
obs-websocket through silence/tone/gap/Restore Defaults/pack-install, a properties-vs-render stress
race, source/filter destroy, and dedicated spatial (orientation + blur extent) and transparency
(alpha convention) checks -- 31 named checks, printed as an armed/expected count so a run that gets
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
`[obs-foxfire]` warn:/error: line in the log outside a short, named allow-list (the
licence-public-key-is-zero warning while `FF_PUBLIC_KEY` is unset, and the two lines the harness's
own symlink-zip pack-install attack produces when it is correctly refused -- see
`ALLOWED_WARNING_TEXTS` in `tools/proof.py`); 2 nothing was armed (OBS never came up). Add `--keep`
to keep the temp sandbox and print its path.

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
