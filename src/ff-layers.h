#pragma once
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <stdbool.h>
#include <stdint.h>
#include "ff-frame.h"
#include "ff-viseme.h"
#include "ff-pack.h"

/* A gradient is a texture2d the SHADER never loads: the engine bakes it from colour stops the
   viewer sets, so one shader serves a whole family of looks without a file per look. */
#define FF_GRAD_MAX 8   /* stops; more than this is a palette, not a gradient */
#define FF_GRAD_LUT 256 /* baked width. 8-bit colour anyway, so finer buys nothing visible. */

/* Graphics-context contract
   ------------------------
   Every function here except ff_renderer_add_properties / ff_renderer_set_defaults /
   ff_renderer_apply_settings touches the graphics subsystem and MUST be called with the
   graphics context held:
     - ff_renderer_create / ff_renderer_load / ff_renderer_destroy are called from the
       source's create/update/destroy wrapped in obs_enter_graphics() ... obs_leave_graphics();
     - ff_renderer_render is called from the source's video_render callback, which libobs
       already runs inside the graphics context (do NOT enter it again there).
   The three settings/properties functions touch no graphics state and may be called from any
   thread that owns the renderer. */

/* One shader uniform of one layer. `builtin` params are fed by the engine every frame and are
   never shown as properties; the rest get their value from `def`, which starts at the shader
   default, is replaced by the preset's override, and is replaced again by the user's setting. */
struct ff_param {
	char name[64];
	enum gs_shader_param_type type;
	gs_eparam_t *ep;
	char label[64], group[64];
	float min, max, step;
	bool has_range;
	float def[4];             /* live value: preset default, then the user's setting if there is one */
	float preset_def[4];      /* the pristine preset value; what "Restore Defaults" must come back to */
	gs_image_file_t *tex;     /* TEXTURE params only: whatever is currently bound */
	/* An ANIMATED WebP, which libobs cannot load at all (ffmpeg has no animated-webp decoder).
	   When this is set it owns the picture and `tex` is NULL: anim_tex is what gets bound. */
	struct ff_webp *anim;
	gs_texture_t *anim_tex;
	char tex_pack[512];       /* the pack's own asset, from <string path="...">; "" if none */
	char tex_user[512];       /* the file the VIEWER picked; "" if none. Wins over tex_pack. */
	bool tex_user_allowed;    /* the shader opted in with <bool user = true;> */
	char list[512];           /* optional <string list = "Name=value;Name=value";>: renders the
	                             knob as a named dropdown instead of a bare number, so a pack can
	                             say "Kick" rather than making the viewer know it means 60 Hz */
	gs_eparam_t *tex_size_ep; /* optional sibling "<name>_size" float2, fed the pixel dimensions:
	                             a shader cannot ask a texture its size (GetDimensions does not
	                             compile here), and a viewer-supplied image can be any aspect. */

	/* Gradient: set when the shader declared <string gradient = "#rrggbb,#rrggbb,...">. The
	   annotation's colour list IS the stop count -- there is no separate count to disagree with
	   it. Stops carry a position each so a viewer can push the ramp around, not only recolour
	   it. The LUT is baked at RENDER time, never here: this struct is filled from
	   ff_renderer_apply_settings, which the contract above says touches no graphics state. */
	int grad_stops; /* 0 = not a gradient; >= 2 otherwise */
	float grad_col[FF_GRAD_MAX][4], grad_pos[FF_GRAD_MAX];
	float grad_col_preset[FF_GRAD_MAX][4], grad_pos_preset[FF_GRAD_MAX];
	gs_texture_t *grad_tex;
	bool grad_dirty; /* the stops changed; rebake before the next draw */

	bool builtin;
};

/* A compiled effect plus its parameter table. `effect == NULL` means the effect failed to
   compile; `error` then holds the compiler message (logged once at load, kept so the UI can
   show it) and the layer is skipped at render time. */
struct ff_layer {
	gs_effect_t *effect;
	char error[512];
	struct ff_param *params;
	size_t nparams;
};

struct ff_renderer {
	struct ff_layer layers[FF_MAX_LAYERS];
	size_t nlayers;
	gs_texrender_t *ping, *pong;
	gs_texture_t *spectrum_tex, *wave_tex;
	gs_texture_t *blank; /* 1x1 transparent; stands in for a NULL input so `image` is never NULL */
	uint32_t width, height;
	float time;
	char pack_dir[512]; /* kept so a texture can be re-resolved after load, when the viewer
	                       picks a different file and only the renderer is in scope */
	uint32_t rng;       /* xorshift32 state; per renderer so two instances do not move in lockstep */
	float rand_instance;
	/* The mouth. Kept per renderer rather than per source because the state it carries -- which
	   shape is up and how long it has been up -- belongs to whatever is being drawn, and two
	   sources on the same audio should each hold their own. Updated once per render, so every
	   layer in a preset sees the same shape on the same frame. */
	struct ff_viseme_state viseme;
	struct ff_viseme_params viseme_params;
	int64_t frames_rendered;
};

/* graphics context required */
struct ff_renderer *ff_renderer_create(void);
/* graphics context required */
void ff_renderer_destroy(struct ff_renderer *r);
/* Logs once, at create, when a GPU object obj (as returned by a gs_*_create call) is NULL --
   so a failure is reported at its source instead of surfacing later as a silent pass-through or a
   null dereference at render time. Any caller that creates a GPU object outside ff_renderer_create
   and then unconditionally dereferences it at render time (ff-props.c's filter capture texrender,
   read at ff-filter.c) must guard it the same way. graphics context required. */
void ff_require(const void *obj, const char *name);
/* (re)loads effects for the preset; releases whatever was loaded before, so it is idempotent.
   Passing a NULL pack or preset leaves the renderer with zero layers.
   Compile errors land in layer.error and the log. graphics context required. */
void ff_renderer_load(struct ff_renderer *r, const struct ff_pack *pack, const struct ff_preset *preset);
/* user overrides from settings, keys "l<idx>.<name>". ff_renderer_load resets every value to the
   preset's, so call this after every load, not only when the settings change. */
void ff_renderer_apply_settings(struct ff_renderer *r, obs_data_t *settings);
/* renders the layer stack; `input` may be NULL (source mode) or the filter's captured target;
   returns the final texture -- NULL only when `r` is NULL or w/h is 0; with no renderable layer it returns
   `input`, or the 1x1 transparent texture when `input` is NULL.

   `progress` is fed to shaders as the builtin `progress`: 0..1 through whatever this source is
   currently DOING, which for an alert is its entrance, hold and exit. It is an explicit argument
   rather than a field on ff_frame because ff_frame is the audio frame and this is not audio, and
   because an explicit argument makes the compiler point at every caller that has to decide what
   it means. The visualizer and the filter pass 0: they are not doing anything with a beginning
   and an end, and a shader reading `progress` there gets a defined 0 rather than a stale value.

   `pair` is the two scenes a TRANSITION is crossing between, bound as the builtins `tex_a` and
   `tex_b`. NULL for every other kind, and a shader that reads them there gets the same 1x1
   transparent texture `image` falls back to rather than a stale or unbound one. It is a separate
   argument from `input` because a transition genuinely has two inputs and neither is "the"
   input -- collapsing them into `image` would make every transition shader guess which it had.

   graphics context required. */
struct ff_pair_tex {
	gs_texture_t *a;
	gs_texture_t *b;
};
gs_texture_t *ff_renderer_render(struct ff_renderer *r, const struct ff_frame *f, float progress, gs_texture_t *input,
				 const struct ff_pair_tex *pair, uint32_t w, uint32_t h, float dt);
/* adds the annotated params of every layer to props (one group per annotation group) */
void ff_renderer_add_properties(struct ff_renderer *r, obs_properties_t *props);
/* seeds the settings' defaults from the current per-param values, so a preset switch shows its
   own values rather than the previous preset's. Needs a loaded renderer, so it belongs in the
   source's update (after ff_renderer_load), not in the instance-free get_defaults callback. */
void ff_renderer_set_defaults(struct ff_renderer *r, obs_data_t *settings);
