#include "ff-layers.h"
#include "ff-pack.h"
#include <plugin-support.h>
#include <graphics/image-file.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Uniform names the engine feeds every frame. They are never exposed as properties, and a pack
   that declares one gets the engine's value, not an author-editable knob. */
static const char *BUILTINS[] = {"ViewProj", "image",    "uv_size",     "time",       "frame_dt",      "level",
				 "peak",     "bass",     "mid",         "treble",     "beat",          "beat_count",
				 "spectrum", "waveform", "layer_index", "rand_frame", "rand_instance",
				 "progress",  NULL};

static bool is_builtin(const char *n)
{
	for (int i = 0; BUILTINS[i]; i++)
		if (!strcmp(BUILTINS[i], n))
			return true;
	return false;
}

static float clamp01(float v)
{
	return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

/* Per-renderer xorshift32 instead of rand(): two sources running the same preset get different
   streams, and nothing here perturbs the process-wide rand() sequence other code may rely on. */
static float rng_next(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return (float)(x >> 8) / (float)0x01000000u; /* top 24 bits -> [0,1) */
}

/* A slider's range is only meaningful when the author gave both ends. */
static void clamp_to_range(struct ff_param *p)
{
	if (!p->has_range || p->min > p->max)
		return;
	if (p->type != GS_SHADER_PARAM_FLOAT && p->type != GS_SHADER_PARAM_INT)
		return;
	if (p->def[0] < p->min)
		p->def[0] = p->min;
	else if (p->def[0] > p->max)
		p->def[0] = p->max;
}

/* gs_effect_get_default_val() returns bmemdup(default_val) and libobs's bmalloc(0) hands back a
   1-byte *uninitialised* block, so an undeclared default would read as garbage. Every read goes
   through these two helpers, which check the stored size first and only then copy. */
static void *default_val_of_size(gs_eparam_t *ep, size_t need)
{
	if (gs_effect_get_default_val_size(ep) < need)
		return NULL;
	return gs_effect_get_default_val(ep);
}

/* Copies a string default into `out`. The precision in "%.*s" bounds the read by the stored size,
   so a value that is not NUL-terminated cannot run off the end. */
static bool default_val_str(gs_eparam_t *ep, char *out, size_t cap)
{
	size_t n = gs_effect_get_default_val_size(ep);
	if (!n || n > 4096)
		return false;
	char *v = gs_effect_get_default_val(ep);
	if (!v)
		return false;
	snprintf(out, cap, "%.*s", (int)n, v);
	bfree(v);
	return out[0] != '\0';
}

/* plain truncating copy: snprintf("%s") into a fixed field trips -Wformat-truncation under -Werror */
static void set_field(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);
	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void read_annotations(gs_eparam_t *ep, struct ff_param *p)
{
	bool has_min = false, has_max = false;
	size_t n = gs_param_get_num_annotations(ep);
	for (size_t i = 0; i < n; i++) {
		gs_eparam_t *a = gs_param_get_annotation_by_idx(ep, i);
		struct gs_effect_param_info ai;
		gs_effect_get_param_info(a, &ai);
		if (ai.type == GS_SHADER_PARAM_STRING) {
			/* sized to the largest field it feeds (p->list), so the only cap an author
			   can hit is that field's own -- a smaller staging buffer would silently cut
			   a list that the struct had room for */
			char s[sizeof p->list];
			if (!default_val_str(a, s, sizeof s))
				continue;
			if (!strcmp(ai.name, "label"))
				set_field(p->label, sizeof p->label, s);
			else if (!strcmp(ai.name, "group"))
				set_field(p->group, sizeof p->group, s);
			else if (!strcmp(ai.name, "list")) {
				/* A cut landing mid-number turns "Snare=200" into "Snare=2", which
				   parses cleanly as a WRONG VALUE rather than being dropped, so the
				   author has to be told. The packs we ship sit around 130 characters,
				   well under this, so a warning here means something really is too
				   long rather than flagging ordinary authoring. */
				if (strlen(s) >= sizeof s - 1)
					obs_log(LOG_WARNING,
						"param '%s': the 'list' annotation is too long and was cut "
						"at %d characters; entries past the cut are missing and the "
						"one it lands in may carry a truncated number",
						p->name, (int)(sizeof s - 1));
				set_field(p->list, sizeof p->list, s);
			}
		} else if (ai.type == GS_SHADER_PARAM_FLOAT) {
			void *v = default_val_of_size(a, sizeof(float));
			if (!v)
				continue;
			float f = *(float *)v;
			if (!strcmp(ai.name, "minimum")) {
				p->min = f;
				has_min = true;
			} else if (!strcmp(ai.name, "maximum")) {
				p->max = f;
				has_max = true;
			} else if (!strcmp(ai.name, "step")) {
				p->step = f;
			}
			bfree(v);
		} else if (ai.type == GS_SHADER_PARAM_INT) {
			void *v = default_val_of_size(a, sizeof(int));
			if (!v)
				continue;
			int iv = *(int *)v;
			if (!strcmp(ai.name, "minimum")) {
				p->min = (float)iv;
				has_min = true;
			} else if (!strcmp(ai.name, "maximum")) {
				p->max = (float)iv;
				has_max = true;
			} else if (!strcmp(ai.name, "step")) {
				p->step = (float)iv;
			}
			bfree(v);
		}
	}
	/* one end alone is not a range: a lone minimum would pin the slider's top to 0 */
	p->has_range = has_min && has_max;
}

/* The default value of a texture2d param is unusable, so packs name their textures with a string
   annotation: uniform texture2d ink <string path="textures/ink.png";>; */
static void free_texture(struct ff_param *p)
{
	if (p->grad_tex) {
		gs_texture_destroy(p->grad_tex);
		p->grad_tex = NULL;
		p->grad_dirty = true; /* so a reused param rebakes rather than binding the blank */
	}
	if (!p->tex)
		return;
	gs_image_file_free(p->tex);
	bfree(p->tex);
	p->tex = NULL;
}

/* Loads one absolute file into p->tex. Graphics context required. */
static bool load_texture_file(struct ff_param *p, const char *full)
{
	p->tex = bzalloc(sizeof(gs_image_file_t));
	gs_image_file_init(p->tex, full);
	gs_image_file_init_texture(p->tex);
	/* `loaded` only means the CPU DECODE worked -- libobs sets it from !!texture_data, and
	   gs_image_file_init_texture leaves it alone when gs_texture_create fails (an image past the
	   backend's maximum dimension, or VRAM exhaustion). Checking `loaded` alone therefore returns
	   success with texture == NULL, the fallback never fires, nothing is logged, and set_params
	   binds NULL -- the exact thing its own comment says must never happen. */
	if (p->tex->loaded && p->tex->texture)
		return true;
	free_texture(p);
	return false;
}

/* Binds whichever image should be showing: the viewer's pick if they made one, otherwise the
   pack's own asset. Graphics context required; called at load and again whenever the viewer
   changes the file.

   The two paths are deliberately judged by DIFFERENT rules, and that asymmetry is the point.
   A pack-relative path is content the pack author wrote, so it goes through ff_rel_ok -- the
   same rule as a manifest path -- because a pack must never be able to reach outside itself.
   The viewer's path is absolute by construction: they chose it in a file dialog, on their own
   machine, pointing at their own file. Running ff_rel_ok over it would refuse every legitimate
   choice, which is a guard failing correct work rather than preventing anything. */
static void ff_param_bind_texture(struct ff_param *p, const char *pack_dir)
{
	free_texture(p);

	if (p->tex_user[0]) {
		if (load_texture_file(p, p->tex_user))
			return;
		obs_log(LOG_WARNING, "image '%s' could not be loaded; using the pack's own art",
			p->tex_user);
		/* fall through to the pack asset rather than rendering nothing: a viewer who picks
		   a file OBS cannot read still gets the preset they paid for */
	}

	if (!p->tex_pack[0])
		return;
	struct dstr full = {0};
	dstr_printf(&full, "%s/%s", pack_dir, p->tex_pack);
	if (!load_texture_file(p, full.array))
		obs_log(LOG_WARNING, "texture '%s' failed to load", full.array);
	dstr_free(&full);
}

/* "#rrggbb" or "#rrggbbaa", with or without the '#'. Returns false rather than guessing: a typo
   in a pack's gradient must be named, not silently rendered as black. */
static bool parse_hex_colour(const char *s, float out[4])
{
	while (*s == ' ')
		s++;
	if (*s == '#')
		s++;
	size_t n = 0;
	while (s[n] && s[n] != ' ' && s[n] != ',')
		n++;
	if (n != 6 && n != 8)
		return false;
	unsigned v[4] = {0, 0, 0, 255};
	for (size_t i = 0; i < n; i += 2) {
		unsigned hi, lo;
		const char *d = "0123456789abcdef";
		const char *ph = strchr(d, (int)tolower((unsigned char)s[i]));
		const char *pl = strchr(d, (int)tolower((unsigned char)s[i + 1]));
		if (!ph || !pl)
			return false;
		hi = (unsigned)(ph - d);
		lo = (unsigned)(pl - d);
		v[i / 2] = hi * 16 + lo;
	}
	for (int i = 0; i < 4; i++)
		out[i] = (float)v[i] / 255.f;
	return true;
}

/* Reads a comma-separated colour list into a param's stops, evenly spaced. Returns the number of
   stops, or 0 if the list is unusable -- in which case the caller must NOT treat the param as a
   gradient, because a gradient with no stops is a black texture with no explanation. */
static int parse_gradient(struct ff_param *p, const char *spec, const char *whose)
{
	int n = 0;
	const char *s = spec;
	while (*s && n < FF_GRAD_MAX) {
		while (*s == ' ' || *s == ',')
			s++;
		if (!*s)
			break;
		if (!parse_hex_colour(s, p->grad_col[n])) {
			obs_log(LOG_WARNING,
				"%s: gradient stop %d is not a #rrggbb or #rrggbbaa colour; "
				"the whole gradient is refused rather than drawn wrong",
				whose, n + 1);
			return 0;
		}
		n++;
		while (*s && *s != ',')
			s++;
	}
	if (*s && n >= FF_GRAD_MAX)
		obs_log(LOG_WARNING, "%s: more than %d gradient stops; the rest are ignored", whose,
			FF_GRAD_MAX);
	if (n < 2) {
		obs_log(LOG_WARNING, "%s: a gradient needs at least 2 colour stops, got %d", whose, n);
		return 0;
	}
	for (int i = 0; i < n; i++)
		p->grad_pos[i] = (float)i / (float)(n - 1);
	return n;
}

static void load_texture_param(struct ff_param *p, const char *pack_dir)
{
	size_t n = gs_param_get_num_annotations(p->ep);
	for (size_t i = 0; i < n; i++) {
		gs_eparam_t *a = gs_param_get_annotation_by_idx(p->ep, i);
		struct gs_effect_param_info ai;
		gs_effect_get_param_info(a, &ai);
		if (!strcmp(ai.name, "user") && ai.type != GS_SHADER_PARAM_BOOL) {
			/* <int user = 1;> or <string user = "true";> otherwise falls through silently:
			   the picker never appears, the pack builds, loads and renders, and the
			   headline feature is just absent with nothing in the log. */
			obs_log(LOG_WARNING,
				"param '%s': the 'user' annotation must be a bool (<bool user = true;>); "
				"it is declared otherwise, so no file picker will be offered",
				p->name);
			continue;
		}
		if (ai.type == GS_SHADER_PARAM_BOOL && !strcmp(ai.name, "user")) {
			void *v = default_val_of_size(a, sizeof(bool));
			if (v) {
				p->tex_user_allowed = *(bool *)v;
				bfree(v);
			}
			continue;
		}
		if (ai.type == GS_SHADER_PARAM_STRING && !strcmp(ai.name, "gradient")) {
			/* The preset may already have supplied its own stops through an override, and
			   those win -- that is the whole point of one shader serving many looks. */
			if (p->grad_stops)
				continue;
			char spec[512];
			char whose[128];
			snprintf(whose, sizeof whose, "param '%s'", p->name);
			if (default_val_str(a, spec, sizeof spec))
				p->grad_stops = parse_gradient(p, spec, whose);
			continue;
		}
		if (ai.type != GS_SHADER_PARAM_STRING || strcmp(ai.name, "path"))
			continue;
		char rel[512];
		if (!default_val_str(a, rel, sizeof rel))
			continue;
		/* Same rule as a manifest path (ff_rel_ok in ff-pack.c). These two checks used to
		   disagree: this one refused only a leading '/' and "..", so a backslash escape or a
		   drive letter passed here while ff-pack.c refused them -- weaker on exactly the
		   platform where those are how you leave a directory. */
		if (!ff_rel_ok(rel)) {
			obs_log(LOG_WARNING, "texture path '%s' refused", rel);
			continue;
		}
		/* the preset's choice wins: apply_override ran first and may already have set this */
		if (!p->tex_pack[0])
			set_field(p->tex_pack, sizeof p->tex_pack, rel);
	}
	if (p->grad_stops) {
		/* A gradient IS the texture. Binding a file here as well would load an image nothing
		   ever samples and, worse, leave whichever won depending on annotation order. */
		if (p->tex_pack[0])
			obs_log(LOG_WARNING,
				"param '%s' is a gradient, so its <string path=\"%s\"> is ignored",
				p->name, p->tex_pack);
		p->grad_dirty = true;
		memcpy(p->grad_col_preset, p->grad_col, sizeof p->grad_col_preset);
		memcpy(p->grad_pos_preset, p->grad_pos, sizeof p->grad_pos_preset);
		return;
	}
	ff_param_bind_texture(p, pack_dir);
}

/* A preset override must match the shape of the uniform it names. The brief's unconditional
   16-byte copy turned `"tint": 0.5` against a float4 into (0.5, 0, 0, 0) -- a transparent black
   tint, silently. A mismatch now keeps the shader default and says so. */
static void apply_override(struct ff_param *p, const struct ff_param_override *o, const char *preset_id,
			   size_t layer_idx)
{
	bool colour = o->is_color != 0;
	bool scalar = p->type == GS_SHADER_PARAM_FLOAT || p->type == GS_SHADER_PARAM_INT ||
		      p->type == GS_SHADER_PARAM_BOOL;
	if (o->is_str) {
		/* A string only means anything for a texture: it picks which of the pack's images this
		   preset draws. Judged by ff_rel_ok exactly like the shader's own <string path=...>
		   annotation -- a preset is pack-authored content, so it must not be able to reach
		   outside the pack any more than the shader can. */
		if (p->type != GS_SHADER_PARAM_TEXTURE)
			obs_log(LOG_WARNING,
				"preset '%s' layer %d: param '%s' was given a string, which only means "
				"something for a texture; using the shader default",
				preset_id, (int)layer_idx, p->name);
		else if (strchr(o->str, '#') || p->grad_stops) {
			/* A colour list, not a path: this is how one gradient shader serves a whole
			   family of presets. Judged by the '#' rather than by p->grad_stops alone
			   because overrides are applied BEFORE the shader's own annotations are read,
			   so grad_stops is still 0 on the first preset to do this. */
			char whose[160];
			snprintf(whose, sizeof whose, "preset '%s' layer %d param '%s'", preset_id,
				 (int)layer_idx, p->name);
			int n = parse_gradient(p, o->str, whose);
			if (n)
				p->grad_stops = n;
		}
		else if (!ff_rel_ok(o->str))
			obs_log(LOG_WARNING, "preset '%s' layer %d: texture path '%s' refused",
				preset_id, (int)layer_idx, o->str);
		else
			set_field(p->tex_pack, sizeof p->tex_pack, o->str);
		return;
	}
	if (colour && p->type == GS_SHADER_PARAM_VEC4)
		memcpy(p->def, o->v, sizeof p->def);
	else if (!colour && scalar)
		p->def[0] = o->v[0];
	else
		obs_log(LOG_WARNING,
			"preset '%s' layer %d: param '%s' override has the wrong type; using the shader default",
			preset_id, (int)layer_idx, p->name);
}

static void read_param_default(struct ff_param *p, const struct ff_layer_def *def, const char *preset_id,
			       size_t layer_idx)
{
	/* A float4 with no author-supplied alpha reads as fully transparent, which looks like a
	   broken preset; seed alpha opaque and let an explicit default overwrite all four. */
	if (p->type == GS_SHADER_PARAM_VEC4)
		p->def[3] = 1.f;

	switch (p->type) {
	case GS_SHADER_PARAM_FLOAT: {
		void *v = default_val_of_size(p->ep, sizeof(float));
		if (v) {
			p->def[0] = *(float *)v;
			bfree(v);
		}
		break;
	}
	case GS_SHADER_PARAM_INT: {
		void *v = default_val_of_size(p->ep, sizeof(int));
		if (v) {
			p->def[0] = (float)*(int *)v;
			bfree(v);
		}
		break;
	}
	case GS_SHADER_PARAM_BOOL: {
		void *v = default_val_of_size(p->ep, sizeof(bool));
		if (v) {
			p->def[0] = *(bool *)v ? 1.f : 0.f;
			bfree(v);
		}
		break;
	}
	case GS_SHADER_PARAM_VEC4: {
		void *v = default_val_of_size(p->ep, 4 * sizeof(float));
		if (v) {
			memcpy(p->def, v, 4 * sizeof(float));
			bfree(v);
		}
		break;
	}
	default:
		break;
	}

	/* preset overrides replace the shader default */
	for (size_t k = 0; k < def->nparams; k++) {
		if (strcmp(def->params[k].name, p->name))
			continue;
		apply_override(p, &def->params[k], preset_id, layer_idx);
		break;
	}
	clamp_to_range(p);
	/* everything above is the preset speaking; from here on p->def may be overwritten by a user
	   setting, so the pristine value is kept beside it rather than recomputed later */
	memcpy(p->preset_def, p->def, sizeof p->preset_def);
}

static void load_layer(struct ff_layer *L, const struct ff_pack *pack, const struct ff_preset *preset, size_t idx)
{
	const struct ff_layer_def *def = &preset->layers[idx];
	struct dstr path = {0};
	dstr_printf(&path, "%s/%s", pack->dir, def->effect_path);
	char *err = NULL;
	L->effect = gs_effect_create_from_file(path.array, &err);
	if (!L->effect) {
		snprintf(L->error, sizeof L->error, "%s", err ? err : "unknown compile error");
		obs_log(LOG_WARNING, "layer '%s' failed to compile: %s", path.array, L->error);
		bfree(err);
		dstr_free(&path);
		return;
	}
	bfree(err); /* a successful compile can still leave warnings behind */

	if (!gs_effect_get_technique(L->effect, "Draw")) {
		/* Compiles cleanly but has nothing render_layer can run every frame. Caught here, once,
		   at load -- not in render_layer, which would otherwise re-log this on every single
		   frame. Treated exactly like a compile failure (error recorded, effect torn down, layer
		   left with L->effect == NULL) so the render loop's existing `if (!L->effect) continue`
		   skips it and add_layer_errors() surfaces it in the properties panel, instead of the
		   layer silently clearing its stage of the chain to blank every frame. */
		snprintf(L->error, sizeof L->error, "no 'Draw' technique");
		obs_log(LOG_WARNING, "layer '%s' failed to load: no 'Draw' technique", path.array);
		gs_effect_destroy(L->effect);
		L->effect = NULL;
		dstr_free(&path);
		return;
	}

	size_t n = gs_effect_get_num_params(L->effect);
	L->params = bzalloc(sizeof(struct ff_param) * (n ? n : 1));
	for (size_t i = 0; i < n; i++) {
		gs_eparam_t *ep = gs_effect_get_param_by_idx(L->effect, i);
		struct gs_effect_param_info info;
		gs_effect_get_param_info(ep, &info);
		struct ff_param *p = &L->params[L->nparams++];
		set_field(p->name, sizeof p->name, info.name);
		set_field(p->label, sizeof p->label, info.name);
		p->type = info.type;
		p->ep = ep;
		p->step = 0.01f;
		p->builtin = is_builtin(info.name);
		if (p->builtin)
			continue;
		read_annotations(ep, p);
		read_param_default(p, def, preset->id, idx);
		if (info.type == GS_SHADER_PARAM_TEXTURE) {
			load_texture_param(p, pack->dir);
			/* "<name>_size" is fed the image's pixel dimensions if the shader declares it.
			   float2 is not user-editable, so this never shows up as a stray control. */
			char sz[96];
			snprintf(sz, sizeof sz, "%s_size", info.name);
			gs_eparam_t *sep = gs_effect_get_param_by_name(L->effect, sz);
			if (sep) {
				struct gs_effect_param_info si;
				gs_effect_get_param_info(sep, &si);
				if (si.type == GS_SHADER_PARAM_VEC2)
					p->tex_size_ep = sep;
				else
					/* found but the wrong type -- float2 vs float4 compiles fine and
					   then never receives the size, so a shader dividing by it
					   divides by zero. Silence here looks identical to a shader that
					   simply did not ask for the size. */
					obs_log(LOG_WARNING,
						"layer %d: '%s' must be float2 to receive the image size; "
						"it is declared otherwise and will not be fed",
						(int)idx, sz);
			}
		}
	}
	dstr_free(&path);
}

static void unload_layers(struct ff_renderer *r)
{
	for (size_t i = 0; i < FF_MAX_LAYERS; i++) {
		struct ff_layer *L = &r->layers[i];
		for (size_t k = 0; k < L->nparams; k++) {
			free_texture(&L->params[k]);
		}
		bfree(L->params);
		if (L->effect)
			gs_effect_destroy(L->effect);
		memset(L, 0, sizeof *L);
	}
	r->nlayers = 0;
}

/* Named so a failure is reported once, at create, instead of being hidden forever by a silent
   pass-through or, worse, a null dereference at render time. Exported (see ff-layers.h) so
   ff_instance_create in ff-props.c can apply the SAME guard to the filter's capture texrender --
   the one GPU object created outside this file that render-time code dereferences unconditionally. */
void ff_require(const void *obj, const char *name)
{
	if (!obj)
		obs_log(LOG_ERROR, "renderer: could not create %s (graphics context held?)", name);
}

struct ff_renderer *ff_renderer_create(void)
{
	struct ff_renderer *r = bzalloc(sizeof(struct ff_renderer));
	r->ping = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	r->pong = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	r->spectrum_tex = gs_texture_create(FF_BANDS, 1, GS_R32F, 1, NULL, GS_DYNAMIC);
	r->wave_tex = gs_texture_create(FF_WAVE, 1, GS_R32F, 1, NULL, GS_DYNAMIC);
	const uint8_t zero[4] = {0, 0, 0, 0};
	const uint8_t *zero_p = zero;
	r->blank = gs_texture_create(1, 1, GS_RGBA, 1, &zero_p, 0);
	ff_require(r->ping, "the ping render target");
	ff_require(r->pong, "the pong render target");
	ff_require(r->spectrum_tex, "the spectrum texture");
	ff_require(r->wave_tex, "the waveform texture");
	ff_require(r->blank, "the blank texture");
	r->rng = (uint32_t)(os_gettime_ns() & 0xFFFFFFFFu);
	if (!r->rng)
		r->rng = 0x9E3779B9u; /* xorshift32 is dead at zero */
	r->rand_instance = rng_next(&r->rng);
	return r;
}

void ff_renderer_destroy(struct ff_renderer *r)
{
	if (!r)
		return;
	unload_layers(r);
	if (r->ping)
		gs_texrender_destroy(r->ping);
	if (r->pong)
		gs_texrender_destroy(r->pong);
	if (r->spectrum_tex)
		gs_texture_destroy(r->spectrum_tex);
	if (r->wave_tex)
		gs_texture_destroy(r->wave_tex);
	if (r->blank)
		gs_texture_destroy(r->blank);
	bfree(r);
}

void ff_renderer_load(struct ff_renderer *r, const struct ff_pack *pack, const struct ff_preset *preset)
{
	if (r && pack)
		set_field(r->pack_dir, sizeof r->pack_dir, pack->dir);
	if (!r)
		return;
	unload_layers(r);
	if (!pack || !preset)
		return;
	size_t n = preset->nlayers > FF_MAX_LAYERS ? FF_MAX_LAYERS : preset->nlayers;
	for (size_t i = 0; i < n; i++)
		load_layer(&r->layers[i], pack, preset, i);
	r->nlayers = n;
}

/* ------------------------------------------------------------------ render */

static void set_builtins(struct ff_renderer *r, struct ff_layer *L, const struct ff_frame *f, gs_texture_t *image,
			 int idx, float dt, float rand_frame, float progress)
{
	for (size_t i = 0; i < L->nparams; i++) {
		struct ff_param *p = &L->params[i];
		if (!p->builtin)
			continue;
		const char *n = p->name;
		if (!strcmp(n, "image")) {
			gs_effect_set_texture(p->ep, image);
		} else if (!strcmp(n, "uv_size")) {
			struct vec2 v = {.x = (float)r->width, .y = (float)r->height};
			gs_effect_set_vec2(p->ep, &v);
		} else if (!strcmp(n, "time")) {
			gs_effect_set_float(p->ep, r->time);
		} else if (!strcmp(n, "frame_dt")) {
			gs_effect_set_float(p->ep, dt);
		} else if (!strcmp(n, "level")) {
			gs_effect_set_float(p->ep, f->level);
		} else if (!strcmp(n, "peak")) {
			gs_effect_set_float(p->ep, f->peak);
		} else if (!strcmp(n, "bass")) {
			gs_effect_set_float(p->ep, f->bass);
		} else if (!strcmp(n, "mid")) {
			gs_effect_set_float(p->ep, f->mid);
		} else if (!strcmp(n, "treble")) {
			gs_effect_set_float(p->ep, f->treble);
		} else if (!strcmp(n, "beat")) {
			gs_effect_set_float(p->ep, f->beat);
		} else if (!strcmp(n, "beat_count")) {
			gs_effect_set_int(p->ep, (int)f->beat_count);
		} else if (!strcmp(n, "spectrum")) {
			gs_effect_set_texture(p->ep, r->spectrum_tex);
		} else if (!strcmp(n, "waveform")) {
			gs_effect_set_texture(p->ep, r->wave_tex);
		} else if (!strcmp(n, "layer_index")) {
			gs_effect_set_int(p->ep, idx);
		} else if (!strcmp(n, "rand_frame")) {
			gs_effect_set_float(p->ep, rand_frame);
		} else if (!strcmp(n, "progress")) {
			/* 0..1 through whatever the source is doing. For a visualizer and a filter
			   this is 0 -- neither has a beginning and an end -- so a shader that reads
			   it there gets a defined value rather than a stale one. */
			gs_effect_set_float(p->ep, progress);
		} else if (!strcmp(n, "rand_instance")) {
			gs_effect_set_float(p->ep, r->rand_instance);
		}
		/* ViewProj is set by libobs from the gs_ortho/matrix stack */
	}
}

/* Bakes the stops into a 1 x FF_GRAD_LUT RGBA8 ramp. Graphics context required -- which is why
   this happens here, at render time, and not in ff_renderer_apply_settings: that one is called
   from the properties thread and the header's contract says it touches no graphics state.

   Stops are used in the order the pack wrote them; a stop whose position is behind the one before
   it is clamped forward rather than reordered, so a pack that writes them out of order gets a
   flat band it can see instead of a silently rearranged ramp. */
static void bake_gradient(struct ff_param *p)
{
	uint8_t lut[FF_GRAD_LUT * 4];
	float pos[FF_GRAD_MAX];
	int n = p->grad_stops;
	if (n < 2)
		return;
	pos[0] = p->grad_pos[0];
	for (int i = 1; i < n; i++)
		pos[i] = p->grad_pos[i] < pos[i - 1] ? pos[i - 1] : p->grad_pos[i];

	int s = 0;
	for (int x = 0; x < FF_GRAD_LUT; x++) {
		float t = (float)x / (float)(FF_GRAD_LUT - 1);
		while (s < n - 2 && t > pos[s + 1])
			s++;
		float span = pos[s + 1] - pos[s];
		/* two stops at the same position are a hard edge, not a divide by zero */
		float f = span > 1e-6f ? (t - pos[s]) / span : (t < pos[s] ? 0.f : 1.f);
		if (f < 0.f)
			f = 0.f;
		if (f > 1.f)
			f = 1.f;
		for (int c = 0; c < 4; c++) {
			float v = p->grad_col[s][c] + (p->grad_col[s + 1][c] - p->grad_col[s][c]) * f;
			if (v < 0.f)
				v = 0.f;
			if (v > 1.f)
				v = 1.f;
			lut[x * 4 + c] = (uint8_t)(v * 255.f + 0.5f);
		}
	}

	if (!p->grad_tex) {
		const uint8_t *data = lut;
		p->grad_tex = gs_texture_create(FF_GRAD_LUT, 1, GS_RGBA, 1, &data, GS_DYNAMIC);
		ff_require(p->grad_tex, "a gradient ramp texture");
	} else {
		gs_texture_set_image(p->grad_tex, lut, FF_GRAD_LUT * 4, false);
	}
	p->grad_dirty = false;
}

static void set_params(struct ff_renderer *r, struct ff_layer *L)
{
	for (size_t i = 0; i < L->nparams; i++) {
		struct ff_param *p = &L->params[i];
		if (p->builtin)
			continue;
		switch (p->type) {
		case GS_SHADER_PARAM_FLOAT:
			gs_effect_set_float(p->ep, p->def[0]);
			break;
		case GS_SHADER_PARAM_INT:
			gs_effect_set_int(p->ep, (int)p->def[0]);
			break;
		case GS_SHADER_PARAM_BOOL:
			gs_effect_set_bool(p->ep, p->def[0] != 0.f);
			break;
		case GS_SHADER_PARAM_VEC4: {
			struct vec4 v = {.x = p->def[0], .y = p->def[1], .z = p->def[2], .w = p->def[3]};
			gs_effect_set_vec4(p->ep, &v);
			break;
		}
		case GS_SHADER_PARAM_TEXTURE:
			if (p->grad_stops) {
				if (p->grad_dirty)
					bake_gradient(p);
				gs_effect_set_texture(p->ep, p->grad_tex ? p->grad_tex : r->blank);
				if (p->tex_size_ep) {
					struct vec2 gz = {.x = (float)FF_GRAD_LUT, .y = 1.f};
					gs_effect_set_vec2(p->tex_size_ep, &gz);
				}
				break;
			}
			/* an unbound texture2d would sample NULL, which is undefined on both backends */
			gs_effect_set_texture(p->ep, p->tex ? p->tex->texture : r->blank);
			if (p->tex_size_ep) {
				/* 1x1 for the blank stand-in, so a shader dividing by it cannot divide
				   by zero when no image is bound */
				struct vec2 sz = {.x = p->tex ? (float)p->tex->cx : 1.f,
						  .y = p->tex ? (float)p->tex->cy : 1.f};
				gs_effect_set_vec2(p->tex_size_ep, &sz);
			}
			break;
		default:
			break;
		}
	}
}

static void render_layer(struct ff_layer *L, gs_texture_t *src, uint32_t w, uint32_t h)
{
	gs_technique_t *t = gs_effect_get_technique(L->effect, "Draw");
	if (!t)
		return;
	size_t passes = gs_technique_begin(t);
	for (size_t p = 0; p < passes; p++) {
		if (gs_technique_begin_pass(t, p)) {
			gs_draw_sprite(src, 0, w, h);
			gs_technique_end_pass(t);
		}
	}
	gs_technique_end(t);
}

gs_texture_t *ff_renderer_render(struct ff_renderer *r, const struct ff_frame *f, float progress,
				 gs_texture_t *input, uint32_t w, uint32_t h, float dt)
{
	static const struct ff_frame silence = {0};
	if (!r || !w || !h)
		return NULL;
	if (!f)
		f = &silence;

	r->width = w;
	r->height = h;
	r->time += dt;
	r->frames_rendered++;
	if (r->spectrum_tex)
		gs_texture_set_image(r->spectrum_tex, (const uint8_t *)f->bands, FF_BANDS * sizeof(float), false);
	if (r->wave_tex)
		gs_texture_set_image(r->wave_tex, (const uint8_t *)f->wave, FF_WAVE * sizeof(float), false);

	/* source mode has no input; the 1x1 transparent texture keeps `image` bound and lets the
	   sprite draw supply UVs for layer 0 */
	const float rand_frame = rng_next(&r->rng); /* one draw per frame, shared by every layer */
	gs_texture_t *prev = input ? input : r->blank;
	if (!r->ping || !r->pong)
		return prev; /* graphics subsystem never handed us the targets; pass the input through */
	gs_texrender_t *dst = r->ping;

	gs_blend_state_push();
	gs_enable_blending(false);
	for (size_t i = 0; i < r->nlayers; i++) {
		struct ff_layer *L = &r->layers[i];
		if (!L->effect)
			continue; /* compile error: skipped, reported in props */
		gs_texrender_reset(dst);
		if (!gs_texrender_begin(dst, w, h))
			break;
		struct vec4 clear = {0};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.f, 0);
		gs_ortho(0.f, (float)w, 0.f, (float)h, -100.f, 100.f);
		set_builtins(r, L, f, prev, (int)i, dt, rand_frame, progress);
		set_params(r, L);
		render_layer(L, prev, w, h);
		gs_texrender_end(dst);
		prev = gs_texrender_get_texture(dst);
		dst = (dst == r->ping) ? r->pong : r->ping;
	}
	gs_blend_state_pop();
	return prev;
}

/* -------------------------------------------------------------- properties */

#define FF_MAX_GROUPS 32
#define FF_MAX_LABELS 128
#define FF_DEFAULT_GROUP "Preset"

struct prop_ctx {
	struct {
		char key[80]; /* the sanitised group key -- what OBS actually keys the group box on */
		obs_properties_t *props;
	} g[FF_MAX_GROUPS];
	size_t ngroups;
	struct {
		size_t gi;
		char label[64];
	} seen[FF_MAX_LABELS];
	size_t nseen;
};

static void sanitise(const char *in, char *out, size_t cap)
{
	size_t j = 0;
	for (size_t i = 0; in[i] && j + 1 < cap; i++) {
		char c = in[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
		out[j++] = ok ? c : '_';
	}
	out[j] = '\0';
}

/* Deduped on the SANITISED key, not the label: "My Look" and "My/Look" sanitise to the same key,
   and adding two groups under one key leaks the second sub-properties object. */
static size_t group_index(struct prop_ctx *c, obs_properties_t *props, const char *name)
{
	const char *label = (name && *name) ? name : FF_DEFAULT_GROUP;
	char key[80], safe[64];
	sanitise(label, safe, sizeof safe);
	snprintf(key, sizeof key, "grp.%s", safe);
	for (size_t i = 0; i < c->ngroups; i++)
		if (!strcmp(c->g[i].key, key))
			return i;
	if (c->ngroups == FF_MAX_GROUPS)
		return 0; /* pathological pack: fold the overflow into the first group */

	obs_properties_t *sub = obs_properties_create();
	obs_properties_add_group(props, key, label, OBS_GROUP_NORMAL, sub);
	set_field(c->g[c->ngroups].key, sizeof c->g[c->ngroups].key, key);
	c->g[c->ngroups].props = sub;
	return c->ngroups++;
}

/* Two layers of the same effect put two identically-labelled knobs in one box; the layer prefix
   is what tells the user which is which. */
static void unique_label(struct prop_ctx *c, size_t gi, size_t layer, const char *label, char *out, size_t cap)
{
	bool dup = false;
	for (size_t i = 0; i < c->nseen && !dup; i++)
		dup = c->seen[i].gi == gi && !strcmp(c->seen[i].label, label);
	if (dup)
		snprintf(out, cap, "L%zu %s", layer, label);
	else
		set_field(out, cap, label);
	if (c->nseen < FF_MAX_LABELS) {
		c->seen[c->nseen].gi = gi;
		set_field(c->seen[c->nseen].label, sizeof c->seen[c->nseen].label, label);
		c->nseen++;
	}
}

static void param_key(char *out, size_t cap, size_t layer, const char *name)
{
	snprintf(out, cap, "l%zu.%s", layer, name);
}

/* Settings store a colour as OBS's 0xAABBGGRR integer; the shader wants four 0..1 floats. */
static uint32_t pack_color(const float v[4])
{
	uint32_t r = (uint32_t)(clamp01(v[0]) * 255.f + 0.5f);
	uint32_t g = (uint32_t)(clamp01(v[1]) * 255.f + 0.5f);
	uint32_t b = (uint32_t)(clamp01(v[2]) * 255.f + 0.5f);
	uint32_t a = (uint32_t)(clamp01(v[3]) * 255.f + 0.5f);
	return r | (g << 8) | (b << 16) | (a << 24);
}

static void unpack_color(uint32_t c, float v[4])
{
	v[0] = (float)(c & 0xFF) / 255.f;
	v[1] = (float)((c >> 8) & 0xFF) / 255.f;
	v[2] = (float)((c >> 16) & 0xFF) / 255.f;
	v[3] = (float)((c >> 24) & 0xFF) / 255.f;
}

/* "Name=value;Name=value" -> a dropdown of named numbers. Entries that do not parse are skipped
   with a warning rather than silently dropped: a typo in a pack's annotation would otherwise show
   up as a short list nobody can explain. Returns false if nothing usable was found, so the caller
   can fall back to the ordinary slider instead of leaving an empty combo box. */
#define FF_LIST_MAX 32

/* Parse FIRST, create the property only if something usable came out.
   The previous version created the list up front and, when every entry was malformed, hid it and
   returned false so the caller could fall back to a slider. That fallback was unreachable: the
   hidden property still occupies `key`, and libobs refuses a duplicate (has_prop -> contains_prop
   walks the topmost parent and recurses through groups), so obs_properties_add_float_slider
   returned NULL and the viewer got NO CONTROL AT ALL -- with nothing in the log but libobs's own
   generic "Property 'x' exists". Verified against libobs, not assumed. */
static bool add_named_list(obs_properties_t *grp, const struct ff_param *p, const char *key, const char *label)
{
	struct {
		char name[96];
		double value;
	} items[FF_LIST_MAX];
	size_t added = 0;

	char buf[sizeof p->list];
	set_field(buf, sizeof buf, p->list);
	char *save = NULL;
	for (char *tok = strtok_r(buf, ";", &save); tok && added < FF_LIST_MAX;
	     tok = strtok_r(NULL, ";", &save)) {
		while (*tok == ' ')
			tok++;
		char *eq = strchr(tok, '=');
		if (!eq || eq == tok) {
			obs_log(LOG_WARNING, "param '%s': list entry '%s' has no Name=value", p->name, tok);
			continue;
		}
		/* the whole entry, kept before the split: cutting at '=' first and then logging `tok`
		   printed the NAME while claiming it had no number, hiding the offending text */
		char whole[128];
		set_field(whole, sizeof whole, tok);
		*eq = '\0';
		const char *num = eq + 1;
		char *endp = NULL;
		double v = os_strtod(num); /* locale-independent: plain strtod reads "0.5" as 0 under a
					      comma-decimal LC_NUMERIC, and the endp check would not fire */
		(void)endp;
		/* os_strtod gives no end pointer, so validate the text itself rather than trusting a
		   silent 0: reject anything that is not a number, and anything with trailing junk. */
		const char *q = num;
		while (*q == ' ')
			q++;
		if (*q == '+' || *q == '-')
			q++;
		bool digits = false;
		while ((*q >= '0' && *q <= '9') || *q == '.') {
			if (*q != '.')
				digits = true;
			q++;
		}
		if (*q == 'e' || *q == 'E') {
			q++;
			if (*q == '+' || *q == '-')
				q++;
			while (*q >= '0' && *q <= '9')
				q++;
		}
		while (*q == ' ')
			q++;
		if (!digits || *q) {
			obs_log(LOG_WARNING, "param '%s': list entry '%s' is not a plain number", p->name,
				whole);
			continue;
		}
		set_field(items[added].name, sizeof items[added].name, tok);
		items[added].value = v;
		added++;
	}
	if (!added)
		return false; /* nothing created, so the caller's ordinary control is free to use `key` */

	obs_property_t *list =
		obs_properties_add_list(grp, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
	for (size_t i = 0; i < added; i++)
		obs_property_list_add_float(list, items[i].name, items[i].value);
	return true;
}

/* "<param key>.c3" / "<param key>.p3" -- one settings key per stop. One key holding all of them
   (a packed string, say) would make Restore Defaults and the undo stack all-or-nothing, and would
   silently change meaning the day the stop count changed. */
static void grad_key(char *out, size_t cap, const char *key, char kind, int stop)
{
	snprintf(out, cap, "%s.%c%d", key, kind, stop);
}

/* A gradient becomes 2N controls: a colour and a position per stop. The position sliders are what
   make it a gradient rather than a palette -- without them a viewer can recolour the ramp but
   never move where the colours land, which is most of what tuning a visualizer actually is. */
static void add_gradient_property(obs_properties_t *grp, const struct ff_param *p, const char *key,
				  const char *label)
{
	for (int s = 0; s < p->grad_stops; s++) {
		char k[128], l[128];
		grad_key(k, sizeof k, key, 'c', s);
		snprintf(l, sizeof l, "%s %d", label, s + 1);
		obs_properties_add_color_alpha(grp, k, l);

		grad_key(k, sizeof k, key, 'p', s);
		snprintf(l, sizeof l, "%s %d at", label, s + 1);
		obs_properties_add_float_slider(grp, k, l, 0.0, 1.0, 0.001);
	}
}

static void add_param_property(obs_properties_t *grp, const struct ff_param *p, const char *key, const char *label)
{
	float step = p->step > 0.f ? p->step : 0.01f;
	if (p->list[0] && (p->type == GS_SHADER_PARAM_FLOAT || p->type == GS_SHADER_PARAM_INT) &&
	    add_named_list(grp, p, key, label))
		return;
	switch (p->type) {
	case GS_SHADER_PARAM_FLOAT:
		if (p->has_range)
			obs_properties_add_float_slider(grp, key, label, p->min, p->max, step);
		else
			obs_properties_add_float(grp, key, label, -1e6, 1e6, 0.01);
		break;
	case GS_SHADER_PARAM_INT: {
		int istep = (int)step;
		if (istep < 1)
			istep = 1;
		if (p->has_range)
			obs_properties_add_int_slider(grp, key, label, (int)p->min, (int)p->max, istep);
		else
			obs_properties_add_int(grp, key, label, -1000000, 1000000, istep);
		break;
	}
	case GS_SHADER_PARAM_BOOL:
		obs_properties_add_bool(grp, key, label);
		break;
	case GS_SHADER_PARAM_VEC4:
		obs_properties_add_color_alpha(grp, key, label);
		break;
	case GS_SHADER_PARAM_TEXTURE:
		/* only reached when the shader opted in -- is_exposed() gates it */
		obs_properties_add_path(grp, key, label, OBS_PATH_FILE,
					/* no *.gif: gs_image_file_t can animate one, but only if the host
					   calls gs_image_file_tick + update_texture every frame, and nothing
					   here does. Offering it would hand back a frozen frame 0 with no
					   explanation -- worse than not offering it. */
					"Images (*.png *.jpg *.jpeg *.bmp *.webp);;All files (*.*)", NULL);
		break;
	default:
		/* VEC2/VEC3, STRING and the matrix types are not user-editable */
		break;
	}
}

static bool is_exposed(const struct ff_param *p)
{
	if (p->builtin)
		return false;
	return p->type == GS_SHADER_PARAM_FLOAT || p->type == GS_SHADER_PARAM_INT || p->type == GS_SHADER_PARAM_BOOL ||
	       p->type == GS_SHADER_PARAM_VEC4 ||
	       (p->type == GS_SHADER_PARAM_TEXTURE && (p->tex_user_allowed || p->grad_stops > 0));
}

/* The render loop skips a layer that failed to load -- compile failure, or a compiled effect
   with no "Draw" technique -- which is what makes the skip visible instead of a black frame with
   no explanation. */
static void add_layer_errors(struct ff_renderer *r, obs_properties_t *props)
{
	for (size_t i = 0; i < r->nlayers; i++) {
		if (!r->layers[i].error[0])
			continue;
		char key[32], idx[16], msg[201];
		snprintf(key, sizeof key, "lerr.%zu", i);
		snprintf(idx, sizeof idx, "%zu", i);
		snprintf(msg, sizeof msg, "%.200s", r->layers[i].error);
		struct dstr text = {0};
		dstr_copy(&text, obs_module_text("Foxfire.Layer.Error"));
		dstr_replace(&text, "%1", idx);
		dstr_replace(&text, "%2", msg);
		obs_property_t *prop = obs_properties_add_text(props, key, text.array, OBS_TEXT_INFO);
		obs_property_text_set_info_type(prop, OBS_TEXT_INFO_ERROR);
		dstr_free(&text);
	}
}

void ff_renderer_add_properties(struct ff_renderer *r, obs_properties_t *props)
{
	if (!r || !props)
		return;
	add_layer_errors(r, props);
	struct prop_ctx ctx = {0};
	for (size_t i = 0; i < r->nlayers; i++) {
		struct ff_layer *L = &r->layers[i];
		for (size_t k = 0; k < L->nparams; k++) {
			struct ff_param *p = &L->params[k];
			if (!is_exposed(p))
				continue;
			char key[96], label[96];
			param_key(key, sizeof key, i, p->name);
			size_t gi = group_index(&ctx, props, p->group);
			unique_label(&ctx, gi, i, p->label, label, sizeof label);
			if (p->grad_stops)
				add_gradient_property(ctx.g[gi].props, p, key, label);
			else
				add_param_property(ctx.g[gi].props, p, key, label);
		}
	}
}

void ff_renderer_set_defaults(struct ff_renderer *r, obs_data_t *settings)
{
	if (!r || !settings)
		return;
	for (size_t i = 0; i < r->nlayers; i++) {
		struct ff_layer *L = &r->layers[i];
		for (size_t k = 0; k < L->nparams; k++) {
			struct ff_param *p = &L->params[k];
			if (!is_exposed(p))
				continue;
			char key[96];
			param_key(key, sizeof key, i, p->name);
			if (p->grad_stops) {
				for (int s = 0; s < p->grad_stops; s++) {
					char gk[128];
					grad_key(gk, sizeof gk, key, 'c', s);
					obs_data_set_default_int(
						settings, gk, (long long)pack_color(p->grad_col_preset[s]));
					grad_key(gk, sizeof gk, key, 'p', s);
					obs_data_set_default_double(settings, gk,
								    (double)p->grad_pos_preset[s]);
				}
				continue;
			}
			/* preset_def, never def: def may already carry a user setting, and recording
			   that as the default is what would make "Restore Defaults" a no-op */
			switch (p->type) {
			case GS_SHADER_PARAM_FLOAT:
				obs_data_set_default_double(settings, key, (double)p->preset_def[0]);
				break;
			case GS_SHADER_PARAM_INT:
				obs_data_set_default_int(settings, key, (long long)p->preset_def[0]);
				break;
			case GS_SHADER_PARAM_BOOL:
				obs_data_set_default_bool(settings, key, p->preset_def[0] != 0.f);
				break;
			case GS_SHADER_PARAM_VEC4:
				obs_data_set_default_int(settings, key, (long long)pack_color(p->preset_def));
				break;
			case GS_SHADER_PARAM_TEXTURE:
				obs_data_set_default_string(settings, key, ""); /* "" = the pack's own art */
				break;
			default:
				break;
			}
		}
	}
}

void ff_renderer_apply_settings(struct ff_renderer *r, obs_data_t *settings)
{
	if (!r || !settings)
		return;
	for (size_t i = 0; i < r->nlayers; i++) {
		struct ff_layer *L = &r->layers[i];
		for (size_t k = 0; k < L->nparams; k++) {
			struct ff_param *p = &L->params[k];
			if (!is_exposed(p))
				continue;
			char key[96];
			param_key(key, sizeof key, i, p->name);
			if (p->grad_stops) {
				/* Per stop, so a viewer who moved one stop keeps it while Restore
				   Defaults on another really does restore that one. */
				for (int s = 0; s < p->grad_stops; s++) {
					char gk[128];
					float col[4], pos;
					grad_key(gk, sizeof gk, key, 'c', s);
					if (obs_data_has_user_value(settings, gk))
						unpack_color((uint32_t)obs_data_get_int(settings, gk), col);
					else
						memcpy(col, p->grad_col_preset[s], sizeof col);
					grad_key(gk, sizeof gk, key, 'p', s);
					pos = obs_data_has_user_value(settings, gk)
						      ? (float)obs_data_get_double(settings, gk)
						      : p->grad_pos_preset[s];
					if (pos < 0.f)
						pos = 0.f;
					if (pos > 1.f)
						pos = 1.f;
					/* rebake only on a real change: apply_settings runs on every
					   settings touch, and the bake is 256 stop-searches */
					if (memcmp(p->grad_col[s], col, sizeof col) != 0 ||
					    p->grad_pos[s] != pos) {
						memcpy(p->grad_col[s], col, sizeof col);
						p->grad_pos[s] = pos;
						p->grad_dirty = true;
					}
				}
				continue;
			}
			if (!obs_data_has_user_value(settings, key)) {
				/* untouched, or cleared by Restore Defaults: put the preset's own value
				   back. Skipping would leave the last user value in p->def forever. */
				memcpy(p->def, p->preset_def, sizeof p->def);
				/* the same reasoning for an image: Restore Defaults has to give the
				   pack's own art back, not leave the viewer's last file bound */
				if (p->type == GS_SHADER_PARAM_TEXTURE && p->tex_user[0]) {
					p->tex_user[0] = '\0';
					ff_param_bind_texture(p, r->pack_dir);
				}
				continue;
			}
			switch (p->type) {
			case GS_SHADER_PARAM_FLOAT:
				p->def[0] = (float)obs_data_get_double(settings, key);
				break;
			case GS_SHADER_PARAM_INT:
				p->def[0] = (float)obs_data_get_int(settings, key);
				break;
			case GS_SHADER_PARAM_BOOL:
				p->def[0] = obs_data_get_bool(settings, key) ? 1.f : 0.f;
				break;
			case GS_SHADER_PARAM_VEC4:
				unpack_color((uint32_t)obs_data_get_int(settings, key), p->def);
				break;
			case GS_SHADER_PARAM_TEXTURE: {
				/* Reload only when the path actually changed. update() runs on every
				   settings touch -- re-decoding the image each time would stall the UI
				   thread on a large PNG for no reason. */
				const char *want = obs_data_get_string(settings, key);
				if (!want)
					want = "";
				if (strcmp(want, p->tex_user) != 0) {
					set_field(p->tex_user, sizeof p->tex_user, want);
					ff_param_bind_texture(p, r->pack_dir);
				}
				break;
			}
			default:
				break;
			}
			clamp_to_range(p); /* a settings file from an older pack can carry an out-of-range value */
		}
	}
}
