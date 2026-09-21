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
  "min_engine": "1.0.0", "licensed": true, "released": 1758412800,
  "kinds": ["visualizer", "effects", "overlay"],
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
always silent.** The engine checks, at apply time, that an override's shape matches the uniform
it's being applied to (`apply_override()` in `src/ff-layers.c`): a colour object only applies
cleanly against a **vector** (`float4`) uniform, and a number/boolean only applies cleanly against
a **scalar** (float/int/bool) uniform. Every other combination — an object against a scalar or a
texture, a number against a vector or a texture — is caught and logged:
`preset '<id>' layer <n>: param '<name>' override has the wrong type; using the shader default` —
named to the preset, the layer index, and the parameter. **This warning is not allowlisted in the
render proof, so it fails a CI/local proof run outright, not just a visual glance.** The shader's
own compiled-in default is kept; nothing is zeroed.

That covers the exact mistake this section warns about: write a colour as a bare array (`"tint":
[1.0, 0.41, 0.3, 1.0]`) against a `float4` uniform and, because there's no array-parsing branch (see
above), it's read as an empty, non-colour override — which lands on a vector target and hits that
mismatch branch, loudly.

The one case that genuinely is silent: that same kind of empty, non-colour override landing on a
**scalar** uniform instead. The apply path doesn't distinguish "a real zero the author wrote" from
"nothing was parsed", so it silently writes `0` with no warning at all. This is the one case worth
checking by eye — everything else, the log tells you exactly what and where.

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

## Letting a viewer supply their own image: the `user` annotation

Add `bool user = true;` to a texture uniform and the properties panel offers a file picker for it,
so a viewer can point the parameter at their own logo or art instead of the pack's:

```hlsl
uniform texture2d logo <string path = "art/logo.png"; bool user = true; string label = "Logo";>;
uniform float2 logo_size;   // fed automatically, see below
```

The `path` annotation is still required — it is what renders when the picker is empty, and what
the engine falls back to if the chosen file cannot be read (with a warning naming the file). A
texture with `user` but no `path` has nothing to show and is refused like any other unbound one.

**The annotation must be a `bool`.** `int user = 1;` or `string user = "true";` compiles fine and
is simply not recognised, so the picker never appears and the pack otherwise builds, loads and
renders — the engine logs a warning naming the parameter rather than letting the feature vanish
silently.

**Feed the shader the image's dimensions.** A shader cannot ask a texture its own size on the
OpenGL backend (`GetDimensions` does not compile there), so if you declare a `float2` uniform
named `<texture name>_size` the engine writes the bound image's pixel dimensions into it every
frame — the pack's own art, or the viewer's file, whichever is in use. Without it a 3:1 banner is
drawn squashed into whatever proportions the pack art happened to have. Declaring `<name>_size` as
anything other than `float2` is warned about and not fed, which would leave a shader dividing by
zero.

Animated GIFs are deliberately not offered by the picker: the engine binds a still frame and
nothing ticks the animation, so one would appear permanently frozen on frame 0.

## Gradients: the `gradient` annotation

A texture uniform can be a ramp the engine bakes instead of an image the pack ships:

```hlsl
uniform texture2d ramp <string gradient = "#ff6a4d,#d9a441,#0b0a0d"; string label = "Flame";>;
...
float4 c = ramp.Sample(linSampler, float2(t, 0.5));   // t in 0..1
```

The annotation's colour list IS the stop count — there is no separate count that can disagree
with it. Colours are `#rrggbb` or `#rrggbbaa`, comma-separated, 2 to 8 of them. The engine bakes
a 256×1 RGBA ramp and binds it to that uniform, so the shader samples it exactly like any other
texture.

**A gradient parameter becomes 2N controls in the properties panel**: a colour picker and a
position slider per stop. The positions are what make it a gradient rather than a palette — a
viewer can push where the colours land, not just recolour them. Defaults are evenly spaced.

**A preset can supply its own stops**, which is how one gradient shader serves a whole family of
looks without a texture file per look:

```json
{ "effect": "effects/bars.effect", "params": { "ramp": "#ffffff,#000000" } }
```

A string param on a texture is normally a path into the pack (see Texture parameters); a string
containing `#` on a gradient uniform is read as stops instead.

**A malformed stop refuses the whole gradient**, loudly, rather than substituting black for the
one it could not read — a half-wrong ramp looks like a design decision. The parameter is then
left unbound and the effect draws whatever an unbound texture gives it, with the reason in the
log naming the parameter and which stop.

Two stops at the same position are a hard edge, not an error. A stop positioned behind the one
before it is clamped forward rather than reordered, so a pack that writes its stops out of order
sees a flat band it can notice instead of a silently rearranged ramp.

A gradient uniform must not also carry `<string path=...>`: the gradient wins and the path is
ignored with a warning, because whichever one bound last would otherwise depend on annotation
order.

## Named choices for a number: the `list` annotation

`string list = "Kick=60;Snare=200;Voice=1000";` turns a float uniform into a dropdown of named
values instead of a slider:

```hlsl
uniform float pulse_hz <string label = "Pulse on"; string list = "Kick=60;Snare=200;Voice=1000";> = 60.0;
```

Entries are `Name=number`, separated by `;`. Entries that are malformed, non-numeric or empty are
dropped with a warning; if *none* parse, no dropdown is created and the parameter keeps its normal
slider rather than losing its control entirely. Numbers are parsed locale-independently, so
`1000.5` means the same thing on a machine whose locale writes decimals with a comma.

The annotation is read through a 256-byte buffer, so a longer string is cut — and a cut landing
mid-number turns `Snare=200` into `Snare=2`, which parses cleanly as the *wrong value*. The engine
warns when it sees a string at that limit; keep lists comfortably under it.

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

These two path kinds fail differently, and the difference matters:

- A **manifest path** (an effect path, a preset thumb) failing any single check above refuses the
  whole pack (see "How a bad manifest fails" above), with the specific reason logged and also
  recorded for the properties panel. The recorded reason text is truncated to 400 characters, and
  the properties panel only ever shows the first 8 refusal reasons from a scan — that cap is shared
  across the *entire* scan (both the pack directory bundled with the plugin and your OBS user
  config's pack directory, added together), not 8 per pack; if more than 8 packs across both
  locations fail, only the first 8 reasons make it to the panel (the rest are still logged in full).
- A **texture `path` annotation** failing the same checks does not propagate anywhere: the engine
  logs `texture path '<path>' refused` and moves on to the next annotation. Nothing is added to the
  pack's error list, and nothing about the layer, the preset, or the pack fails — only that one
  texture parameter stays unbound (sampling a 1x1 transparent placeholder), exactly like a texture
  file that's missing or fails to load (see Texture parameters, above).

## Presets and OBS types

`kind` decides which OBS object can use the preset:

| `kind` | used by |
|---|---|
| `visualizer`, `overlay` | the Foxfire Visualizer **source** (obs-foxfire) |
| `effects` | the Foxfire Effects **filter** (obs-foxfire) |
| `alert` | the Foxfire Alert **source** (obs-foxfire-alerts) |

Same manifest, same layer format, `kind` is just the routing — except an invalid `kind` (anything
other than those four strings, matched exactly and in lower case) doesn't just fail to route, it
refuses the whole pack the same as any other manifest error (see "How a bad manifest fails"
above).

**One list, every plugin.** Foxfire ships as separate plugins, but they all load packs through
the same loader, so a pack carrying `alert` presets stays valid on a machine where only the
visualizer is installed — it is simply not offered there. Refusing the whole pack would take its
visualizer presets down with it.

### Alert presets and `progress`

An alert has a beginning and an end, and the engine tells the shader where it is:

```hlsl
uniform float progress;   /* 0 at the start of the alert, 1 at the end */
...
float appear = smoothstep(0.0, 0.14, progress);
float leave  = 1.0 - smoothstep(0.82, 1.0, progress);
```

That is the whole entrance-and-exit animation, decided by the pack. There is no list of four
transitions to choose from, which is what every hosted alert service offers instead.

`progress` is a builtin like `level` or `beat`, so it is never a property. The visualizer and the
filter feed it **0** — neither is doing something with a beginning and an end — so a shader that
reads it there gets a defined value rather than a stale one.

An alert source draws the pack's layers first and composites the **name** on top of them, so a
preset should leave room for text rather than filling the frame with detail.

A preset's `thumb` field is optional — omit it, or leave it `""`, and the engine skips validating it
entirely. Set it and it's checked exactly like an effect path (see Refused paths, above).

## Licensed packs: `released`, and a broken licence loads zero layers

This is the single most consequential behaviour in this document for anyone shipping a *paid* pack,
so it gets its own section rather than a footnote.

**A pack someone bought never stops working.** There is no expiry, no grace period and no clock
check anywhere in the load path — a licence is not a lease. What a licence carries is
`entitled_through`, the last moment the buyer was paying, and what a pack carries is `released`,
the moment that *version of the pack* was published. The whole model is one comparison:

```
released <= entitled_through   ->  it renders
released >  entitled_through   ->  it does not
```

So a buyer keeps every pack that existed while they were subscribed, forever, offline, on a
machine whose clock is wrong — and a pack we publish *after* they stopped paying does not open.
Nothing they already had is ever taken away.

**`released` is mandatory for a licensed pack and there is no safe default.** It is a Unix
timestamp in seconds. `0`, a missing key, a quoted string like `"1758412800"`, a boolean, a list —
all of those read back as `0` through libobs's `obs_data_get_int`, and `0 <= entitled_through` is
true for every licence ever issued, so each one would silently unlock a paid pack for anybody.
The engine therefore refuses the pack outright if `licensed` is true and `released` is missing,
the wrong type, or not greater than zero. `packforge` refuses to build or ship such a pack for the
same reason. A pack with `"licensed": false` may omit `released` entirely.

If `licence.json` is **missing, unreadable, or fails signature verification**, or if the pack
post-dates what the buyer paid for, the engine does not degrade gracefully and does not fall back
to the shader defaults — it loads that pack with **zero layers**. Every preset in it renders
nothing at all: a transparent source, a no-op filter, with a reason shown in the properties panel
(source status / info line) but no visual output whatsoever. The exact same pack with
`"licensed": false` renders completely normally. This was measured directly, not inferred from
reading the gating code.

The states that block are checked in `src/ff-props.c` (`licence_blocks()`): `NEWER` (the licence
verifies, but `released` is past `entitled_through`), `INVALID` (includes "the file doesn't
exist"), and `NONE` (declared `licensed: true` but no licence was ever read for it) all zero the
layers out. Only `OK` renders. There is no intermediate tier that renders with a warning.

The practical upshot for packaging a paid pack: ship it with `licensed: false` while you're testing
render output, and only flip it to `true` once `licence.json` is in place and verified — a paid pack
built with an empty or placeholder `licence.json` doesn't fail loudly, it just doesn't render
anything, and it will look exactly like a build that shipped broken.

## Where packs live

Two places, scanned in this order:

1. **Bundled** — `data/packs/` inside the plugin itself. This is where the free pack that ships
   with the engine lives. The first pack found wins on an id clash, so a bundled pack shadows an
   installed one with the same id.
2. **Installed** — `<OBS config>/plugin_config/foxfire/packs/<pack id>/`, which on Linux is
   `~/.config/obs-studio/plugin_config/foxfire/packs/`.

**Note the `foxfire` in that path, not the plugin's own name.** Foxfire ships as several separate
plugins so nobody has to install all of it to use one part, and OBS's own
`obs_module_config_path` is per plugin — which would give each one its own packs directory, and
make a pack installed through one invisible to the others. The path is rewritten to a fixed
`foxfire` component so every Foxfire plugin reads and writes the same place. The "Install pack
(.zip)" button puts packs there too; the two halves have to agree, or the button reports success
having written somewhere nothing reads.

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

## Mouth shapes: `viseme` and `mouth_open`

Two more builtins, for a mouth overlaid on a character:

```c
uniform float viseme;     /* which shape to draw, 0-8 */
uniform float mouth_open; /* how open, 0..1, smoothed */
```

`viseme` numbers the classic Preston Blair set, in the order the art is conventionally drawn:

| # | Shape | For |
| --- | --- | --- |
| 0 | A | closed, slight pressure — P, B, M |
| 1 | B | slightly open, teeth together — "EE", K, S, T, and every fricative |
| 2 | C | open — "EH", "AE" |
| 3 | D | wide open — "AA" as in *father* |
| 4 | E | slightly rounded — "AO", "ER" |
| 5 | F | puckered — "UW", "OW", W |
| 6 | G | teeth on lip — F, V — **never returned** |
| 7 | H | tongue raised — long L — **never returned** |
| 8 | X | rest, relaxed closed |

**The engine returns A–F and X, and nothing else.** G and H are in the numbering because the
Preston Blair set has them and a strip drawn for Rhubarb should still work, but Foxfire will
never select either: both are articulatory rather than spectral (see `src/ff-viseme.h` for why),
and getting them means running a phoneme recogniser, which Rhubarb does offline over a finished
file. **Six cells is the set worth drawing.**

A shader still has to answer for the indices it was not given, because a pack may ship four cells
or nine. The fallbacks are X→A, G→B, H→C. `packs/mouth/effects/mouth.effect` has this as
`ff_cell()`; copy it rather than clamping, because clamping sends every rest frame to whatever
happens to be the last cell in the strip.

The shape is decided in C, not in the shader, because choosing it needs memory of the previous
frame — how long the current shape has been up. Without that hold the mouth changes on every
ambiguous frame and reads as flapping rather than speech. A shader has no frame memory, which is
why this arrives as a number rather than as something a pack works out for itself.

**Art:** one image, the shapes side by side left to right in the order above, same cell size,
mouth registered in the same place in every cell, transparent background. The engine feeds the
bound image's pixel dimensions as `mouth_size`, so a shader can work out one cell's aspect and
avoid squashing every mouth by the number of cells.

**Registration is the requirement that bites.** The engine swaps cells frame to frame, so any
drift in where the mouth sits between them reads as the mouth sliding around the face while the
character talks — and six cells that each look right in a contact sheet will still slide. It is
also the thing generated art gets wrong: six image generations are six independent mouths, and
frames pulled from a video model drift as the head moves. Measure it instead of judging by eye:

```
tools/check-mouth-strip.py art/mouth-strip.png            # reports drift per cell
tools/check-mouth-strip.py --fix out.png raw.png          # aligns them
tools/check-mouth-strip.py A.png B.png C.png D.png E.png F.png
```

It anchors on the mouth **corners**, which sit on the line where the lips meet and stay put while
the jaw works — not the bounding box or the centroid, both of which move down as the mouth opens.
Anchoring on either would call a correct strip broken and then "fix" it by shoving every open
shape upward.

`tools/make-mouth-strip.py` draws the examples the pack ships (`--style flat`, `--style ink`) and
a registration template to draw over (`--style guide`).

### Generating a strip from a rendered animation

Six independent image generations are six independent mouths. A video model is a better fit,
because temporal coherence is the thing it is trained for — but then the question becomes which
frame is which shape, and answering that by eye is the same judgement that puts drift into
hand-aligned art. So it gets measured:

```
# 1. render, 2. cut the backgrounds off, then:
tools/pick-mouth-frames.py --out raw.png close/*.png pucker/*.png
tools/check-mouth-strip.py --fix strip.png raw.png
```

**Cut the background off before measuring.** The strip has to be transparent anyway — an opaque
one draws a rectangle over the character's face — so this step happens either way; it just has to
happen first, because nothing can find where the lips end in a frame that is opaque everywhere.
Measured rather than refused, an opaque clip reports every frame as equally wide and the rounded
shapes become unfindable; the picker exits 2 and names the first offending frame instead.

**Start the render from an open mouth**, showing teeth and tongue. A shut mouth has no interior,
so a model starting there invents one — differently each time. Every other shape is a subset of
detail that already exists in an open frame.

**Two clips, not one.** `open → closed` gives D, C, B and A; `neutral → pucker` gives E and F.
E and F are not small versions of C and D: rounding the lips pulls the *corners* in, so what
separates them is width, not aperture, and no closing clip contains them however many frames it
has. The picker measures both axes and says `nothing close` for any shape the clips do not hold.

The clips must contain at least one wide, unrounded frame, because lip widths are normalised
against the widest one present.

`pick-mouth-frames.py --selftest` renders an animation whose right answer is known by
construction and checks the picks against it; `--mutate` breaks the picker four ways and shows
the selftest catching each.

### Two ways to drive a mouth

`viseme` swaps between drawn shapes; `mouth_open` stretches one drawing. The `mouth` pack ships
both, because they want different art and suit different rigs:

| Preset | Reads | Art it needs |
| --- | --- | --- |
| Mouth — sprite strip | `viseme` | six cells, A–F |
| Mouth — one image, stretched | `mouth_open` | one drawing of an **open** mouth |

The stretched one cannot tell an "oo" from an "ee" — nothing about how far the jaw is down can —
but it is the rig most people already have, and the one to start from before six shapes exist.

Two things its shader does that are worth copying. It scales about the **top** of the art, not
the centre: the upper lip barely moves when a jaw opens, and a mouth that grows about its middle
climbs toward the nose every time it shuts. And it scales **height only** — a uniform scale looks
correct in a still and reads as the mouth receding into the face when it moves.

The art should be an **open** mouth, cropped to its own bounds. Open, because this only ever
scales down from it, and a closed drawing stretched tall is a yawn rather than speech; cropped,
because the top edge of the image is the pivot, so it has to be the top of the upper lip.

### The Mouth controls

A preset that declares either builtin also gets a **Mouth** group in the properties panel, added
by the engine rather than by the pack. Nothing here is a shader uniform — these four are the
timing the classifier runs on, and they describe the voice and the microphone rather than the art:

| Control | Default | What it does |
| --- | --- | --- |
| Silence threshold | 0.04 | Below this the mouth stops answering. Breath, a fan and a keyboard all have energy; a mouth that answers them looks possessed. A quiet talker needs it lower. |
| Closed-mouth gap (ms) | 200 | A gap **shorter** than this is a stop consonant — the closure in P, B, M — and the mouth shuts. Longer, and the speaker has stopped, so the mouth rests. |
| Minimum shape time (ms) | 80 | How long a shape stays up once chosen. The number that decides whether this reads as speech or as flapping; roughly the length of a spoken phoneme. |
| Mouth close speed (ms) | 120 | How fast `mouth_open` falls. Opening is immediate either way — a mouth that lags the attack of a word looks dubbed. |
| Jaw bias | 0.00 | Shifts both jaw thresholds together: positive opens the mouth more readily, negative keeps it shut. Openness is the one feature measured **not** to be portable between recordings — its median moves 0.156 across four real voices, against 0.08 between the thresholds — so this is the control a streamer is most likely to need. |

They are **not** keyed per layer, so switching preset keeps them: a streamer who dialled a mouth
in to their own voice should not have to do it again to try different art.

## The shared controls: Placement and Response

Every Foxfire shader that **draws a shape** carries the same block of controls, with the same
names, ranges and meanings:

| Group | Controls |
| --- | --- |
| Placement | `pos_x`, `pos_y`, `size`, `rotate_deg`, `opacity` |
| Response | `gain`, `smoothing`, `punch` — only where the shader reads the spectrum |

This is a convention, not something the engine enforces, and it exists because moving from one
preset to another should not mean relearning the panel.

**Do not type these out.** OBS's effect language has no `#include` (this format refuses one — see
*Refused paths* — because an include does not resolve into a pack directory), so the block is
genuinely copied into each shader. Generate the copies:

```
tools/apply-placement.py --reference <a shader that has it> --blocks place,shape,response <target.effect>
```

and check them with `tools/check-placement.py <pack dir>`, which fails on a control declared two
different ways, and on a helper that is defined and never called. Both tools live in the engine
repo.

### What the block gives you

* `ff_place(uv, aspect)` turns canvas UV into the shape's own space, applying position, rotation
  and size. Rotation happens in **square** units, before the size divide, or a rotated shape shears
  on any canvas that is not square.
* `ff_on_shape(uv)` is 1 inside the shape's own 0..1 box and 0 outside, so shrinking a shape leaves
  empty canvas instead of tiling the pattern across the frame.
* `ff_band(hL, hC, hR, beat)` reads one spectrum band with its neighbours blended in and gain and
  beat punch applied. The three samples are taken by the **caller** and passed as floats: taking
  `texture2d` and `sampler_state` parameters is the obvious way to write it and does not survive
  OBS's HLSL→GLSL translation.

### A shader may decline any of them, and should

The checker compares **per control**, not per block, precisely so that it can:

* A shader that reads a single level rather than a spectrum has **no `smoothing`** — there are no
  neighbouring bands to blend, and the knob would do nothing.
* A shape bounded by its own geometry — a ring, a rounded rectangle — needs **no `ff_on_shape`**.
  Masking it to the square box would only square off a shape that grew past the edge.
* A layer that **processes what is below it** (a grain overlay, a glow pass) gets **no Placement at
  all**. Moving it means nothing, and its own one control is the control.
* A shader reading a **signed** waveform applies gain and smoothing inline rather than through
  `ff_band`, which clamps to 0..1 and would flatten the bottom half of the wave.

`smoothing` is **spatial** — it blends a band with its neighbours. A shader has no memory of the
previous frame, so a control that claimed to smooth over time would do nothing.

### Alert presets do not take the block

An alert source is a source of its own, so OBS's Transform already moves it, and it draws its art
**under** a text child that the source positions itself — shifting the art in the shader would
slide the card out from under its own text. The alert source also builds its properties panel by
hand, so a knob declared in an alert shader never reaches a streamer at all: everything in one is
set by the preset, by you.

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
