# Pack format (version 1)

This is the format for authors: what a pack directory looks like, what `pack.json` declares, and
exactly what the engine checks before it will load any of it. Where this document and the design
spec disagree, **this document describes what the code actually enforces** — the code wins, and
each place that happened is called out below.

## Layout

```
<pack-id>/
  pack.json        manifest (below)
  licence.json     paid packs only: signed, per buyer
  licensee.json    paid packs only: visible buyer record {discord_id, display_name, order, issued}
  LICENSE.txt      paid packs only: KitsuneStudio pack licence
  effects/*.effect
  textures/*.png   static textures only in v1 (animated loops ship as separate OBS Media Source files)
  thumbs/*.png     per-preset thumbnails (optional — see Presets below)
```

## `pack.json`

```json
{ "format": 1, "id": "ember", "name": "Ember", "version": "1.0.0", "author": "KitsuneStudio",
  "min_engine": "1.0.0", "licensed": true, "kinds": ["visualizer", "effects", "overlay"],
  "presets": [ { "id": "ember-bars", "name": "Ember Bars", "kind": "visualizer",
                 "thumb": "thumbs/ember-bars.png", "heavy": false,
                 "layers": [ { "effect": "effects/bars.effect",
                               "params": { "glow": 0.6, "tint": {"r": 1, "g": 0.41, "b": 0.3, "a": 1} } },
                             { "effect": "effects/grain.effect", "params": { "amount": 0.15 } } ] } ] }
```

`"format"` must be the integer `1` — anything else (missing, a string, `2`, ...) refuses the pack
before any other field is even read.

Note: the engine reads and validates each preset's own `"kind"`; it does not read or check the
top-level `"kinds"` array at all. Keep it accurate for humans and any future tooling, but it is not
enforced today — a preset whose `kind` disagrees with the manifest's `kinds` list will load anyway.

## How a bad manifest fails — read this before the sections below

**Every check in this document, if it fails, refuses the whole pack — not just the offending
preset or layer.** The loader (`load_pack_dir()` in `src/ff-pack.c`) builds up the pack's presets
one at a time; the moment any one of them fails to parse (bad id, bad kind, bad path, an
out-of-range layer count, and so on), the loader logs the reason, frees everything it had built so
far, and returns failure for the *entire pack directory*. None of that pack's presets load — not
just the one that was wrong.

Concretely: if your pack has nine presets and the ninth has a 9-layer preset (one over the limit),
none of the other eight show up either. An author debugging "why did my other presets vanish" should
look for a single bad preset, not assume something is wrong with the presets that disappeared.

The one thing that's scoped narrower than the whole pack: a single texture that fails to load inside
an otherwise-valid layer (see the `path` annotation section below) — that leaves one parameter
unbound rather than failing the pack, the preset, or even the layer.

## The colour-object rule

Colours are objects, **never** bare-number arrays:

```json
"tint": {"r": 1.0, "g": 0.41, "b": 0.3, "a": 1.0}
```

not

```json
"tint": [1.0, 0.41, 0.3, 1.0]
```

This isn't a style preference — libobs' `obs_data` JSON parser cannot parse an array of bare
numbers at all. There is no array-parsing branch in the engine's manifest loader (`src/ff-pack.c`)
for override values; a param override is read as either an object (colour, all four of `r`/`g`/`b`
mapped in, `a` optional and defaulting to `1.0` if omitted), a number, or a boolean.

**What actually happens if you get this wrong depends on the uniform's own type, and it is not
always silent.** The engine separately checks, at apply time, that an override's shape matches the
uniform it's being applied to (`apply_override()` in `src/ff-layers.c`):

- Write a colour as a bare array against a **vector** uniform (a `float4` tint, exactly the mistake
  this section warns about) and the override doesn't parse into r/g/b/a at all, so it's read as a
  non-colour, wrong-shape value against a vector target. The engine catches that mismatch and logs
  `preset '<id>' layer <n>: param '<name>' override has the wrong type; using the shader default` —
  named to the preset, the layer index, and the parameter. **This warning is not allowlisted in the
  render proof, so it fails a CI/local proof run outright, not just a visual glance.** The shader's
  own compiled-in default is kept; nothing is zeroed.
- The same wrong-type warning fires for any other shape mismatch against a **non-scalar** uniform
  (texture, vector) — an object where a scalar was expected, a number against a texture, and so on.
- The one case that genuinely is silent: a malformed override (such as a bare array, which parses to
  nothing) against a **scalar** uniform (float/int/bool). There the apply path doesn't distinguish
  "a real zero the author wrote" from "nothing was parsed", so it silently writes `0` with no
  warning at all. This is the one case worth checking by eye — everything else, the log tells you
  exactly what and where.

## Texture parameters: the `path` annotation

A texture uniform is bound by a string annotation named `path` on the uniform itself:

```hlsl
uniform texture2d ink <string path="textures/ink.png";>;
```

The engine reads exactly the annotation named `path` (`src/ff-layers.c`, `load_texture_param`) —
not `file`, not `texture`, not a `params` entry in `pack.json`. Texture bindings live in the
shader, not the manifest.

A texture `path` annotation is judged by the same rule as every other relative path in a pack (see
Refused paths, below) — refused if it's absolute, contains `..`, a backslash, or a colon. Write it
exactly like an effect path: forward slashes, no leading `/`, no `..`.

Either way, the referenced file must exist inside the pack directory — a texture that fails to load
logs a warning and leaves that parameter unbound (sampling a 1x1 transparent placeholder); it does
not fail the whole layer, preset, or pack.

## Layer and preset bounds

- A preset needs **1 to 8 layers** (`FF_MAX_LAYERS` in `src/ff-pack.h`). 0 or 9+ layers refuses the
  whole pack — see "How a bad manifest fails" above — with a message naming which preset and the
  count it got.
- A pack needs **1 to 64 presets** (`FF_MAX_PRESETS`) declared in the manifest's `presets` array. 0
  or 65+ refuses the pack.

## Ids

Both the pack's own `id` and every preset's `id` must match `[a-z0-9-]+` and be no more than 63
characters. Anything else (uppercase, underscores, spaces, unicode) refuses the pack as a whole —
a bad preset id doesn't just drop that preset, it takes every other preset in the pack down with it
(see "How a bad manifest fails" above). This isn't just a style rule either: the id becomes a
directory/key name elsewhere in the engine.

## `min_engine`

`min_engine` must be an exact `x.y.z` string — three integers, dot-separated, and *nothing* else
trailing (`1.0.0-beta`, `1.0`, `1.0.0.1` all fail to parse and refuse the pack). The engine compares
it against its own running version and refuses to load a pack that asks for a newer engine than the
one that's running. There's no upper-bound check and no bypass: a pack with a malformed
`min_engine` string is refused the same as one that's genuinely too new.

## Refused paths, summarized

Any relative path in `pack.json` (an effect path, a preset thumb) or in a shader's texture `path`
annotation is refused if it:

- starts with `/` (absolute)
- contains `..` (parent traversal)
- contains `\` (backslash — a Windows-style separator has no meaning inside a pack)
- contains `:` (colon — rules out `C:\...` and similar)
- does not exist as a real file inside the pack directory once resolved

A pack that fails any single check above is refused as a whole (see "How a bad manifest fails"
above), with the specific reason logged and also recorded for the properties panel. The recorded
reason text is truncated to 400 characters, and the properties panel only ever shows the first 8
refusal reasons from a scan — that cap is shared across the *entire* scan (both the pack directory
bundled with the plugin and your OBS user config's pack directory, added together), not 8 per pack;
if more than 8 packs across both locations fail, only the first 8 reasons make it to the panel (the
rest are still logged in full).

## Presets and OBS types

`kind` decides which OBS object type can use the preset: `visualizer` and `overlay` presets show up
for the Foxfire Visualizer *source*; `effects` presets show up for the Foxfire Effects *filter*.
Same manifest, same layer format, `kind` is just the routing — except an invalid `kind` (anything
other than those three strings) doesn't just fail to route, it refuses the whole pack the same as
any other manifest error (see "How a bad manifest fails" above).

A preset's `thumb` field is optional — omit it, or leave it `""`, and the engine skips validating it
entirely. Set it and it's checked exactly like an effect path (see Refused paths, above).

## Licensed packs: a broken licence loads zero layers, not a degraded pack

This is the single most consequential behaviour in this document for anyone shipping a *paid* pack,
so it gets its own section rather than a footnote.

Set `"licensed": true` and the engine looks for `licence.json` next to `pack.json` at load time. If
that file is **missing, unreadable, fails signature verification, or has expired past its grace
period**, the engine does not degrade gracefully and does not fall back to the shader defaults — it
loads that pack with **zero layers**. Every preset in it renders nothing at all: a transparent
source, a no-op filter, with a reason shown in the properties panel (source status / info line) but
no visual output whatsoever. The exact same pack with `"licensed": false` renders completely
normally. This was measured directly, not inferred from reading the gating code.

The states that block are checked in `src/ff-props.c` (`licence_blocks()`): `EXPIRED`, `INVALID`
(includes "the file doesn't exist"), and `NONE` (declared `licensed: true` but no licence was ever
read for it) all zero the layers out. `OK` and `GRACE` (expired but still inside the renewal grace
window) do **not** block — those still render, `GRACE` with a warning shown alongside.

The practical upshot for packaging a paid pack: ship it with `licensed: false` while you're testing
render output, and only flip it to `true` once `licence.json` is in place and verified — a paid pack
built with an empty or placeholder `licence.json` doesn't fail loudly, it just doesn't render
anything, and it will look exactly like a build that shipped broken.

## Installing as a `.zip`

Authors and buyers both end up installing a pack from a `.zip` (via the plugin's "Install pack
(.zip)" button, `ff_packs_install_zip()` in `src/ff-pack.c`). The zip itself is checked before its
contents are ever handed to the manifest loader above:

- The zip must contain **exactly one top-level folder** — every entry inside it must start with
  `<top>/`. A zip built from *inside* the pack directory (so its entries start with `pack.json`,
  `effects/…` directly, no wrapping folder) is refused, as is a zip with more than one top-level
  folder.
- That top-level folder's name must be made only of the same characters a pack/preset id is allowed
  (`[a-z0-9-]`) — a capitalised folder name, spaces, or anything else refuses the whole zip before
  any entry is even extracted.
- No entry anywhere in the zip may contain a `..` path segment, checked per-entry (not just on the
  top-level name), so nothing in the zip can extract outside the pack's own directory.

Only once all of that passes does the engine extract the zip and run it through the exact same
manifest validation described everywhere else in this document. A zip that passes these structural
checks but fails manifest validation (a bad `min_engine`, too many layers, whatever) is refused with
that validation's own reason, exactly as if it had been dropped straight into the packs folder.

## Shader construct compatibility — Linux/OpenGL

Foxfire's `.effect` shaders build for both Direct3D (Windows) and OpenGL (Linux/macOS), and OBS's
shader translator does not accept everything HLSL does on both backends. KitsuneStudio holds a
measured, private study of which constructs actually break, and how — ask for a copy before writing
a shader meant to ship.

The two most important findings for an author, because they fail **silently** — the shader compiles,
nothing is logged, and the preset renders a plain black frame with no error anywhere to point at:

- **A shader that never uses `ViewProj`.** The engine supplies `ViewProj` from the OBS matrix stack;
  a vertex shader that doesn't multiply by it never transforms its geometry into clip space, so
  nothing lands on screen. The engine has no way to detect this — a shader that legitimately doesn't
  need per-vertex transform (there isn't one) looks identical to a shader that forgot the uniform.
  Every layer's vertex shader must multiply the incoming position by `ViewProj`, following the demo
  pack's `VSDefault` function.
- **A technique not named `Draw`.** The engine looks up the technique literally named `Draw`
  (`gs_effect_get_technique(effect, "Draw")`) and does nothing else if that lookup fails. As of this
  engine version this case is no longer silent: the engine logs a warning naming the effect path at
  load time and treats the layer as failed (skipped in render, reported in the source's properties
  panel), the same as a shader that fails to compile. It's still worth getting right the first time
  — a failed layer still means a black gap in your preset until you fix and reload it.

Everything else that study covers — casts like `(float)x`, `tex2D()`, `%` on floats, `#include`,
`ddx_coarse`, `.GetDimensions()` — fails loudly: the effect fails to compile, the engine logs the
compiler's own error naming the file, and the layer is skipped exactly like the missing-technique
case above. Those are easy to find and fix. The two above are the ones that cost real debugging
time, because nothing tells you where to look.
