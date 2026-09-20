#include "ff-layers.h"
#include <plugin-support.h>
#include <graphics/image-file.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <string.h>
#include <stdlib.h>

/* Uniform names the engine feeds every frame. They are never exposed as properties, and a pack
   that declares one gets the engine's value, not an author-editable knob. */
static const char *BUILTINS[] = {"ViewProj", "image",  "uv_size",     "time",      "frame_dt",   "level",
				 "peak",     "bass",   "mid",         "treble",    "beat",       "beat_count",
				 "spectrum", "waveform", "layer_index", "rand_frame", "rand_instance", NULL};

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
	size_t n = gs_param_get_num_annotations(ep);
	for (size_t i = 0; i < n; i++) {
		gs_eparam_t *a = gs_param_get_annotation_by_idx(ep, i);
		struct gs_effect_param_info ai;
		gs_effect_get_param_info(a, &ai);
		if (ai.type == GS_SHADER_PARAM_STRING) {
			char s[256];
			if (!default_val_str(a, s, sizeof s))
				continue;
			if (!strcmp(ai.name, "label"))
				set_field(p->label, sizeof p->label, s);
			else if (!strcmp(ai.name, "group"))
				set_field(p->group, sizeof p->group, s);
		} else if (ai.type == GS_SHADER_PARAM_FLOAT) {
			void *v = default_val_of_size(a, sizeof(float));
			if (!v)
				continue;
			float f = *(float *)v;
			if (!strcmp(ai.name, "minimum")) {
				p->min = f;
				p->has_range = true;
			} else if (!strcmp(ai.name, "maximum")) {
				p->max = f;
				p->has_range = true;
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
				p->has_range = true;
			} else if (!strcmp(ai.name, "maximum")) {
				p->max = (float)iv;
				p->has_range = true;
			} else if (!strcmp(ai.name, "step")) {
				p->step = (float)iv;
			}
			bfree(v);
		}
	}
}

/* The default value of a texture2d param is unusable, so packs name their textures with a string
   annotation: uniform texture2d ink <string path="textures/ink.png";>; */
static void load_texture_param(struct ff_param *p, const char *pack_dir)
{
	size_t n = gs_param_get_num_annotations(p->ep);
	for (size_t i = 0; i < n; i++) {
		gs_eparam_t *a = gs_param_get_annotation_by_idx(p->ep, i);
		struct gs_effect_param_info ai;
		gs_effect_get_param_info(a, &ai);
		if (ai.type != GS_SHADER_PARAM_STRING || strcmp(ai.name, "path"))
			continue;
		char rel[512];
		if (!default_val_str(a, rel, sizeof rel))
			continue;
		if (rel[0] == '/' || strstr(rel, "..")) {
			obs_log(LOG_WARNING, "texture path '%s' refused", rel);
			continue;
		}
		struct dstr full = {0};
		dstr_printf(&full, "%s/%s", pack_dir, rel);
		p->tex = bzalloc(sizeof(gs_image_file_t));
		gs_image_file_init(p->tex, full.array);
		gs_image_file_init_texture(p->tex);
		if (!p->tex->loaded) {
			obs_log(LOG_WARNING, "texture '%s' failed to load", full.array);
			gs_image_file_free(p->tex);
			bfree(p->tex);
			p->tex = NULL;
		}
		dstr_free(&full);
		return;
	}
}

static void read_param_default(struct ff_param *p, const struct ff_layer_def *def)
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
	for (size_t k = 0; k < def->nparams; k++)
		if (!strcmp(def->params[k].name, p->name))
			memcpy(p->def, def->params[k].v, sizeof p->def);
}

static void load_layer(struct ff_layer *L, const struct ff_pack *pack, const struct ff_layer_def *def)
{
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
		read_param_default(p, def);
		if (info.type == GS_SHADER_PARAM_TEXTURE)
			load_texture_param(p, pack->dir);
	}
	dstr_free(&path);
}

static void unload_layers(struct ff_renderer *r)
{
	for (size_t i = 0; i < FF_MAX_LAYERS; i++) {
		struct ff_layer *L = &r->layers[i];
		for (size_t k = 0; k < L->nparams; k++) {
			if (L->params[k].tex) {
				gs_image_file_free(L->params[k].tex);
				bfree(L->params[k].tex);
			}
		}
		bfree(L->params);
		if (L->effect)
			gs_effect_destroy(L->effect);
		memset(L, 0, sizeof *L);
	}
	r->nlayers = 0;
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
	r->rand_instance = (float)rand() / (float)RAND_MAX;
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
	if (!r)
		return;
	unload_layers(r);
	if (!pack || !preset)
		return;
	size_t n = preset->nlayers > FF_MAX_LAYERS ? FF_MAX_LAYERS : preset->nlayers;
	for (size_t i = 0; i < n; i++)
		load_layer(&r->layers[i], pack, &preset->layers[i]);
	r->nlayers = n;
}

/* ------------------------------------------------------------------ render */

static void set_builtins(struct ff_renderer *r, struct ff_layer *L, const struct ff_frame *f, gs_texture_t *image,
			 int idx, float dt)
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
			gs_effect_set_float(p->ep, (float)rand() / (float)RAND_MAX);
		} else if (!strcmp(n, "rand_instance")) {
			gs_effect_set_float(p->ep, r->rand_instance);
		}
		/* ViewProj is set by libobs from the gs_ortho/matrix stack */
	}
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
			/* an unbound texture2d would sample NULL, which is undefined on both backends */
			gs_effect_set_texture(p->ep, p->tex ? p->tex->texture : r->blank);
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

gs_texture_t *ff_renderer_render(struct ff_renderer *r, const struct ff_frame *f, gs_texture_t *input, uint32_t w,
				 uint32_t h, float dt)
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
	gs_texture_t *prev = input ? input : r->blank;
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
		set_builtins(r, L, f, prev, (int)i, dt);
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
#define FF_DEFAULT_GROUP "Preset"

struct group_table {
	struct {
		char name[64];
		obs_properties_t *props;
	} g[FF_MAX_GROUPS];
	size_t n;
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

static obs_properties_t *group_for(struct group_table *tab, obs_properties_t *props, const char *name)
{
	const char *label = (name && *name) ? name : FF_DEFAULT_GROUP;
	for (size_t i = 0; i < tab->n; i++)
		if (!strcmp(tab->g[i].name, label))
			return tab->g[i].props;
	if (tab->n == FF_MAX_GROUPS)
		return tab->g[0].props; /* pathological pack: fold the overflow into the first group */

	char key[80], safe[64];
	sanitise(label, safe, sizeof safe);
	snprintf(key, sizeof key, "grp.%s", safe);
	obs_properties_t *sub = obs_properties_create();
	obs_properties_add_group(props, key, label, OBS_GROUP_NORMAL, sub);
	set_field(tab->g[tab->n].name, sizeof tab->g[tab->n].name, label);
	tab->g[tab->n].props = sub;
	tab->n++;
	return sub;
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

static void add_param_property(obs_properties_t *grp, const struct ff_param *p, const char *key)
{
	float step = p->step > 0.f ? p->step : 0.01f;
	switch (p->type) {
	case GS_SHADER_PARAM_FLOAT:
		if (p->has_range)
			obs_properties_add_float_slider(grp, key, p->label, p->min, p->max, step);
		else
			obs_properties_add_float(grp, key, p->label, -1e6, 1e6, 0.01);
		break;
	case GS_SHADER_PARAM_INT: {
		int istep = (int)step;
		if (istep < 1)
			istep = 1;
		if (p->has_range)
			obs_properties_add_int_slider(grp, key, p->label, (int)p->min, (int)p->max, istep);
		else
			obs_properties_add_int(grp, key, p->label, -1000000, 1000000, istep);
		break;
	}
	case GS_SHADER_PARAM_BOOL:
		obs_properties_add_bool(grp, key, p->label);
		break;
	case GS_SHADER_PARAM_VEC4:
		obs_properties_add_color_alpha(grp, key, p->label);
		break;
	default:
		/* TEXTURE, VEC2/VEC3, STRING and the matrix types are not user-editable */
		break;
	}
}

static bool is_exposed(const struct ff_param *p)
{
	if (p->builtin)
		return false;
	return p->type == GS_SHADER_PARAM_FLOAT || p->type == GS_SHADER_PARAM_INT ||
	       p->type == GS_SHADER_PARAM_BOOL || p->type == GS_SHADER_PARAM_VEC4;
}

void ff_renderer_add_properties(struct ff_renderer *r, obs_properties_t *props)
{
	if (!r || !props)
		return;
	struct group_table tab = {0};
	for (size_t i = 0; i < r->nlayers; i++) {
		struct ff_layer *L = &r->layers[i];
		for (size_t k = 0; k < L->nparams; k++) {
			struct ff_param *p = &L->params[k];
			if (!is_exposed(p))
				continue;
			char key[96];
			param_key(key, sizeof key, i, p->name);
			add_param_property(group_for(&tab, props, p->group), p, key);
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
			switch (p->type) {
			case GS_SHADER_PARAM_FLOAT:
				obs_data_set_default_double(settings, key, (double)p->def[0]);
				break;
			case GS_SHADER_PARAM_INT:
				obs_data_set_default_int(settings, key, (long long)p->def[0]);
				break;
			case GS_SHADER_PARAM_BOOL:
				obs_data_set_default_bool(settings, key, p->def[0] != 0.f);
				break;
			case GS_SHADER_PARAM_VEC4:
				obs_data_set_default_int(settings, key, (long long)pack_color(p->def));
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
			if (!obs_data_has_user_value(settings, key))
				continue; /* untouched: the preset's own value stands */
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
			default:
				break;
			}
		}
	}
}
