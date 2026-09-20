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
  thumbs/*.png     per-preset thumbnails
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

Note: the engine reads and validates each preset's own `"kind"`; it does not read or check the
top-level `"kinds"` array at all. Keep it accurate for humans and any future tooling, but it is not
enforced today — a preset whose `kind` disagrees with the manifest's `kinds` list will load anyway.

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
mapped in, `a` optional and defaulting to `1.0` if omitted), a number, or a boolean. Anything else
is silently read as the zeroed default — there is no error for a malformed override, so check your
JSON by eye.

## Texture parameters: the `path` annotation

A texture uniform is bound by a string annotation named `path` on the uniform itself:

```hlsl
uniform texture2d ink <string path="textures/ink.png";>;
```

The engine reads exactly the annotation named `path` (`src/ff-layers.c`, `load_texture_param`) —
not `file`, not `texture`, not a `params` entry in `pack.json`. Texture bindings live in the
shader, not the manifest.

One enforcement gap worth knowing about: the manifest's layer/thumb/effect paths are checked by a
stricter rule than a texture's `path` annotation is. `pack.json`-level paths (effect files, preset
thumbnails) are refused if they start with `/`, contain `..`, contain a backslash, or contain a
colon (`rel_ok()` in `src/ff-pack.c`). A texture's `path` annotation is only checked for a leading
`/` or a `..` component (`load_texture_param()` in `src/ff-layers.c`) — a backslash or a colon in a
texture path is not refused by that check. Don't rely on this; write texture paths the same way you
write effect paths (forward slashes, no leading `/`, no `..`).

Either way, the referenced file must exist inside the pack directory — a texture that fails to load
logs a warning and leaves that parameter unbound (sampling a 1x1 transparent placeholder), it does
not fail the whole layer.

## Layer and preset bounds

- A preset needs **1 to 8 layers** (`FF_MAX_LAYERS` in `src/ff-pack.h`). 0 or 9+ layers refuses the
  whole preset with a message naming the count it got.
- A pack needs **1 to 64 presets** (`FF_MAX_PRESETS`). 0 or 65+ refuses the whole pack.

## Ids

Both the pack's own `id` and every preset's `id` must match `[a-z0-9-]+` and be no more than 63
characters. Anything else (uppercase, underscores, spaces, unicode) refuses the pack/preset outright
— this isn't just a style rule, the id becomes a directory/key name elsewhere in the engine.

## `min_engine`

`min_engine` must be an exact `x.y.z` string — three integers, dot-separated, and *nothing* else
trailing (`1.0.0-beta`, `1.0`, `1.0.0.1` all fail to parse and refuse the pack). The engine compares
it against its own running version and refuses to load a pack that asks for a newer engine than the
one that's running. There's no upper-bound check and no bypass: a pack with a malformed
`min_engine` string is refused the same as one that's genuinely too new.

## Refused paths, summarized

Any relative path in `pack.json` (an effect path, a preset thumb) is refused if it:

- starts with `/` (absolute)
- contains `..` (parent traversal)
- contains `\` (backslash — a Windows-style separator has no meaning inside a pack)
- contains `:` (colon — rules out `C:\...` and similar)
- does not exist as a real file inside the pack directory once resolved

A pack that fails any single check above is refused as a whole, with the specific reason logged and
also recorded for the properties panel (up to the first 8 pack-level errors and the first 512
characters of each preset-level reason).

## Presets and OBS types

`kind` decides which OBS object type can use the preset: `visualizer` and `overlay` presets show up
for the Foxfire Visualizer *source*; `effects` presets show up for the Foxfire Effects *filter*.
Same manifest, same layer format, `kind` is just the routing.

## Shader construct compatibility — Linux/OpenGL

Foxfire's `.effect` shaders build for both Direct3D (Windows) and OpenGL (Linux/macOS), and OBS's
shader translator does not accept everything HLSL does on both backends. A measured study of which
constructs actually break, and how, is written up at
`~/vault/projects/foxfire/research/shader-compat.md` (private repo — ask KitsuneStudio if you need a
copy for pack development). Read it before writing a shader meant to ship.

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

Everything else the shader-compat study covers — casts like `(float)x`, `tex2D()`, `%` on floats,
`#include`, `ddx_coarse`, `.GetDimensions()` — fails loudly: the effect fails to compile, the engine
logs the compiler's own error naming the file, and the layer is skipped exactly like the
missing-technique case above. Those are easy to find and fix. The two above are the ones that cost
real debugging time, because nothing tells you where to look.
