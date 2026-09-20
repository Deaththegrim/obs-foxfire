/*
Foxfire
Copyright (C) 2026 KitsuneStudio ninjaflashboy@gmail.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "ff-props.h"
#include <plugin-support.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ helpers */

/* Substitutes a single "%1" in a localised string. The locale file is the only place the
   sentences live, so every caller goes through here rather than composing prose in C. */
static void fmt1(char *out, size_t cap, const char *key, const char *a)
{
	struct dstr t = {0};
	dstr_copy(&t, obs_module_text(key));
	dstr_replace(&t, "%1", a ? a : "");
	snprintf(out, cap, "%s", t.array ? t.array : "");
	dstr_free(&t);
}

static void fmt_date(int64_t t, char *out, size_t cap)
{
	time_t tt = (time_t)t;
	struct tm tm;
	if (!gmtime_r(&tt, &tm) || strftime(out, cap, "%Y-%m-%d", &tm) == 0)
		snprintf(out, cap, "?");
}

static void refresh_packs(struct ff_instance *in)
{
	ff_packs_free(&in->packs);
	ff_packs_scan(&in->packs, (int64_t)time(NULL));
}

/* The renderer keys its per-parameter settings "l<idx>.<name>", so those keys belong to whichever
   preset was loaded when they were written. Switching preset must take them with it: left behind,
   ff_renderer_apply_settings would feed one shader's value to the same-named knob of another. */
static bool is_layer_key(const char *n)
{
	if (!n || n[0] != 'l' || !isdigit((unsigned char)n[1]))
		return false;
	size_t i = 1;
	while (isdigit((unsigned char)n[i]))
		i++;
	return n[i] == '.';
}

static void erase_layer_keys(obs_data_t *s)
{
	DARRAY(char *) doomed;
	da_init(doomed);
	/* names are collected first: erasing while iterating would invalidate the cursor */
	obs_data_item_t *item = obs_data_first(s);
	for (; item; obs_data_item_next(&item)) {
		const char *n = obs_data_item_get_name(item);
		if (!is_layer_key(n))
			continue;
		char *copy = bstrdup(n);
		da_push_back(doomed, &copy);
	}
	for (size_t i = 0; i < doomed.num; i++) {
		obs_data_erase(s, doomed.array[i]);
		bfree(doomed.array[i]);
	}
	da_free(doomed);
}

/* The one place a licence state becomes a sentence: both the Status line (when the state refuses
   the load) and the Licence line read it, so the two can never disagree. */
static void licence_sentence(const struct ff_pack *pk, char *out, size_t cap)
{
	char date[32];
	if (!pk || !pk->licensed) {
		snprintf(out, cap, "%s", obs_module_text("Foxfire.Licence.None"));
		return;
	}
	struct dstr t = {0};
	switch (pk->licence.state) {
	case FF_LIC_OK:
		fmt_date(pk->licence.expires, date, sizeof date);
		dstr_copy(&t, obs_module_text("Foxfire.Licence.OK"));
		dstr_replace(&t, "%1", pk->licensee_name[0] ? pk->licensee_name : pk->licence.discord_id);
		dstr_replace(&t, "%2", date);
		break;
	case FF_LIC_GRACE:
		fmt_date(pk->licence.expires + FF_LIC_GRACE_SECONDS, date, sizeof date);
		dstr_copy(&t, obs_module_text("Foxfire.Licence.Grace"));
		dstr_replace(&t, "%1", date);
		break;
	case FF_LIC_EXPIRED:
		fmt_date(pk->licence.expires, date, sizeof date);
		dstr_copy(&t, obs_module_text("Foxfire.Licence.Expired"));
		dstr_replace(&t, "%1", date);
		break;
	case FF_LIC_INVALID:
		if (!strcmp(pk->licence.reason, "missing")) {
			dstr_copy(&t, obs_module_text("Foxfire.Licence.Missing"));
		} else {
			dstr_copy(&t, obs_module_text("Foxfire.Licence.Invalid"));
			dstr_replace(&t, "%1", pk->licence.reason);
		}
		break;
	default:
		/* NONE on a pack that declares licensed:true means no licence was ever read for it;
		   ff-pack.c never leaves it that way, and if it ever did the pack must not load. */
		dstr_copy(&t, obs_module_text("Foxfire.Licence.Missing"));
		break;
	}
	snprintf(out, cap, "%s", t.array ? t.array : "");
	dstr_free(&t);
}

/* Spec §2/§3.8: expired or invalid (including missing) loads ZERO layers; grace loads normally. */
static bool licence_blocks(const struct ff_pack *pk)
{
	if (!pk || !pk->licensed)
		return false;
	return pk->licence.state == FF_LIC_EXPIRED || pk->licence.state == FF_LIC_INVALID ||
	       pk->licence.state == FF_LIC_NONE;
}

/* --------------------------------------------------------------- list filling */

static void populate_pack_list(struct ff_instance *in, obs_property_t *p)
{
	if (!p)
		return;
	obs_property_list_clear(p);
	for (size_t i = 0; i < in->packs.n; i++)
		obs_property_list_add_string(p, in->packs.packs[i].name, in->packs.packs[i].id);
}

/* A source draws its own canvas (visualizer/overlay presets); a filter reworks another source's
   image (effects presets). Offering the wrong kind would load a stack that has nothing to read. */
static bool preset_kind_matches(const struct ff_preset *pr, bool is_filter)
{
	if (is_filter)
		return !strcmp(pr->kind, "effects");
	return !strcmp(pr->kind, "visualizer") || !strcmp(pr->kind, "overlay");
}

static void populate_preset_list(struct ff_instance *in, obs_property_t *p, const char *pack_id)
{
	if (!p)
		return;
	obs_property_list_clear(p);
	const struct ff_pack *pk = ff_packs_find(&in->packs, pack_id ? pack_id : "");
	if (!pk)
		return;
	for (size_t i = 0; i < pk->npresets; i++)
		if (preset_kind_matches(&pk->presets[i], in->is_filter))
			obs_property_list_add_string(p, pk->presets[i].name, pk->presets[i].id);
}

static bool add_audio_source(void *data, obs_source_t *src)
{
	obs_property_t *p = data;
	const char *name = obs_source_get_name(src);
	if (name && (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO))
		obs_property_list_add_string(p, name, name);
	return true;
}

static void populate_audio_sources(obs_property_t *p)
{
	obs_property_list_clear(p);
	obs_enum_sources(add_audio_source, p);
}

/* ------------------------------------------------------------------ callbacks */

static bool on_pack_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	struct ff_instance *in = obs_properties_get_param(props);
	if (!in)
		return false;
	obs_property_t *presets = obs_properties_get(props, S_PRESET);
	populate_preset_list(in, presets, obs_data_get_string(s, S_PACK));
	/* the outgoing pack's preset id rarely exists in the incoming one; land on its first entry
	   rather than leaving a selection that resolves to nothing */
	const char *cur = obs_data_get_string(s, S_PRESET);
	size_t n = obs_property_list_item_count(presets);
	bool found = false;
	for (size_t i = 0; i < n && !found; i++)
		found = !strcmp(obs_property_list_item_string(presets, i), cur);
	if (!found && n)
		obs_data_set_string(s, S_PRESET, obs_property_list_item_string(presets, 0));
	/* no obs_properties_apply_settings() here: it runs every modified callback, this one
	   included, and would recurse without end. Returning true is what refreshes the page. */
	return true;
}

static bool on_preset_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(s);
	return true; /* the new preset has its own parameter widgets: make OBS rebuild the page */
}

static bool on_audio_mode_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	obs_property_t *src = obs_properties_get(props, S_AUDIO_SOURCE);
	if (src)
		obs_property_set_visible(src, obs_data_get_int(s, S_AUDIO_MODE) == FF_AUDIO_SOURCE);
	return true;
}

static bool on_install_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	struct ff_instance *in = obs_properties_get_param(props);
	const char *zip = obs_data_get_string(s, S_INSTALL);
	if (!in || !zip || !*zip)
		return false;
	char msg[512] = {0};
	bool ok = ff_packs_install_zip(zip, msg, sizeof msg);
	obs_log(ok ? LOG_INFO : LOG_WARNING, "install '%s': %s: %s", zip, ok ? "ok" : "refused", msg);
	/* its own field, not in->status: the source update that follows this callback runs on the
	   video thread and rewrites status, which would swallow a refusal before it was ever shown */
	snprintf(in->install_msg, sizeof in->install_msg, "%s", msg);
	in->install_failed = !ok;
	/* one shot: left set, the picker would reinstall on every later update() */
	obs_data_set_string(s, S_INSTALL, "");
	refresh_packs(in);
	populate_pack_list(in, obs_properties_get(props, S_PACK));
	populate_preset_list(in, obs_properties_get(props, S_PRESET), obs_data_get_string(s, S_PACK));
	return true;
}

static bool on_reload(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	struct ff_instance *in = data;
	if (!in)
		return false;
	refresh_packs(in);
	/* the authoring loop: the ids have not changed but the files on disk have, so ask update()
	   for a reload without pretending the preset switched (that would wipe the user's knobs) */
	in->reload_pending = true;
	/* libobs defers a video source's update to the video thread; calling ff_instance_update from
	   here would run it on two threads at once, so the flag is what carries the request */
	obs_source_update(in->self, NULL);
	populate_pack_list(in, obs_properties_get(props, S_PACK));
	populate_preset_list(in, obs_properties_get(props, S_PRESET), in->pack_id);
	return true;
}

/* -------------------------------------------------------------- status lines */

static void add_info(obs_properties_t *props, const char *key, const char *text, enum obs_text_info_type type)
{
	obs_property_t *p = obs_properties_add_text(props, key, text, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, type);
}

/* Spec §3.9: every refusal reaches the user as one sentence here. The list is never empty, so a
   working instance is visibly working rather than indistinguishable from a silent one. */
static void add_status_lines(struct ff_instance *in, obs_properties_t *props)
{
	bool any = false;
	if (in->status[0]) {
		add_info(props, "status", in->status, OBS_TEXT_INFO_WARNING);
		any = true;
	}
	if (in->install_msg[0]) {
		add_info(props, "status.install", in->install_msg,
			 in->install_failed ? OBS_TEXT_INFO_ERROR : OBS_TEXT_INFO_NORMAL);
		any = true;
	}
	char amsg[256];
	if (!ff_audio_status(in->audio, amsg, sizeof amsg)) {
		add_info(props, "status.audio", amsg, OBS_TEXT_INFO_WARNING);
		any = true;
	}
	for (size_t i = 0; i < in->packs.nerrors; i++) {
		char key[32];
		snprintf(key, sizeof key, "status.pack%zu", i);
		add_info(props, key, in->packs.errors[i], OBS_TEXT_INFO_WARNING);
		any = true;
	}
	if (!any)
		add_info(props, "status", obs_module_text("Foxfire.Status.OK"), OBS_TEXT_INFO_NORMAL);
}

static void add_licence_line(struct ff_instance *in, obs_properties_t *props)
{
	const struct ff_pack *pk = ff_packs_find(&in->packs, in->pack_id);
	if (!pk)
		return; /* no pack to describe; the Status line already says which one is missing */
	char line[320];
	licence_sentence(pk, line, sizeof line);
	enum obs_text_info_type type = licence_blocks(pk) ? OBS_TEXT_INFO_ERROR
				       : (pk && pk->licensed && pk->licence.state == FF_LIC_GRACE)
					       ? OBS_TEXT_INFO_WARNING
					       : OBS_TEXT_INFO_NORMAL;
	add_info(props, "licence", line, type);
}

static void add_heavy_line(struct ff_instance *in, obs_properties_t *props)
{
	const struct ff_pack *pk = ff_packs_find(&in->packs, in->pack_id);
	const struct ff_preset *pr = ff_pack_find_preset(pk, in->preset_id);
	if (pr && pr->heavy)
		add_info(props, "heavy", obs_module_text("Foxfire.Preset.Heavy"), OBS_TEXT_INFO_WARNING);
}

/* ------------------------------------------------------------------ lifecycle */

struct ff_instance *ff_instance_create(obs_data_t *settings, obs_source_t *self, bool is_filter)
{
	struct ff_instance *in = bzalloc(sizeof *in);
	in->self = self;
	in->is_filter = is_filter;
	in->audio = ff_audio_create();
	refresh_packs(in);
	obs_enter_graphics();
	in->renderer = ff_renderer_create();
	if (is_filter)
		in->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	ff_instance_update(in, settings);
	return in;
}

void ff_instance_destroy(struct ff_instance *in)
{
	if (!in)
		return;
	/* audio first: it stops publishing frames before the renderer that reads them goes away */
	ff_audio_destroy(in->audio);
	obs_enter_graphics();
	ff_renderer_destroy(in->renderer);
	if (in->capture)
		gs_texrender_destroy(in->capture);
	obs_leave_graphics();
	ff_packs_free(&in->packs);
	bfree(in);
}

void ff_instance_update(struct ff_instance *in, obs_data_t *s)
{
	if (!in || !s)
		return;
	const char *pack = obs_data_get_string(s, S_PACK);
	const char *preset = obs_data_get_string(s, S_PRESET);
	in->width = (uint32_t)obs_data_get_int(s, S_WIDTH);
	in->height = (uint32_t)obs_data_get_int(s, S_HEIGHT);

	struct ff_analysis_params ap = {
		.release_ms = (float)obs_data_get_double(s, S_RELEASE),
		.beat_sensitivity = (float)obs_data_get_double(s, S_BEAT),
		.gain_db = (float)obs_data_get_double(s, S_GAIN),
	};
	ff_audio_configure(in->audio, (enum ff_audio_mode)obs_data_get_int(s, S_AUDIO_MODE),
			   obs_data_get_string(s, S_AUDIO_SOURCE), &ap);

	in->status[0] = 0;
	const struct ff_pack *pk = ff_packs_find(&in->packs, pack);
	const struct ff_preset *pr = ff_pack_find_preset(pk, preset);
	if (!pk) {
		fmt1(in->status, sizeof in->status, "Foxfire.Pack.Missing", pack);
		obs_log(LOG_WARNING, "pack '%s' is not installed; rendering nothing", pack);
	} else if (licence_blocks(pk)) {
		licence_sentence(pk, in->status, sizeof in->status);
		obs_log(LOG_WARNING, "pack '%s': %s", pack, in->status);
		pr = NULL; /* the gate: a blocked pack loads zero layers */
	} else if (!pr) {
		fmt1(in->status, sizeof in->status, "Foxfire.Preset.Missing", preset);
		obs_log(LOG_WARNING, "pack '%s' has no preset '%s'; rendering nothing", pack, preset);
	}

	/* On the first update there is no previous preset, so nothing is stale: treating a scene
	   load as a preset switch would erase the layer values the user saved with the scene. */
	bool changed = in->initialised && (strcmp(in->pack_id, pack) != 0 || strcmp(in->preset_id, preset) != 0);
	snprintf(in->pack_id, sizeof in->pack_id, "%s", pack);
	snprintf(in->preset_id, sizeof in->preset_id, "%s", preset);
	if (changed)
		erase_layer_keys(s);

	/* set_defaults/apply_settings touch no graphics state, but video_render holds this same
	   mutex for the whole frame: taking it here is what keeps them from rewriting a param
	   halfway through a draw. */
	obs_enter_graphics();
	if (changed || in->reload_pending || !in->initialised || !in->renderer->nlayers)
		ff_renderer_load(in->renderer, pk, pr); /* a NULL preset leaves zero layers */
	ff_renderer_set_defaults(in->renderer, s);      /* so the UI shows this preset's own values */
	ff_renderer_apply_settings(in->renderer, s);
	obs_leave_graphics();
	in->reload_pending = false;
	in->initialised = true;
}

void ff_instance_defaults(obs_data_t *s, bool is_filter)
{
	obs_data_set_default_string(s, S_PACK, "demo");
	obs_data_set_default_string(s, S_PRESET, is_filter ? "glow-only" : "bars");
	obs_data_set_default_int(s, S_WIDTH, 1920);
	obs_data_set_default_int(s, S_HEIGHT, 1080);
	obs_data_set_default_int(s, S_AUDIO_MODE, FF_AUDIO_MASTER);
	obs_data_set_default_string(s, S_AUDIO_SOURCE, "");
	obs_data_set_default_double(s, S_GAIN, 0.0);
	obs_data_set_default_double(s, S_RELEASE, 150.0);
	obs_data_set_default_double(s, S_BEAT, 1.0);
	obs_data_set_default_string(s, S_INSTALL, "");
}

obs_properties_t *ff_instance_properties(struct ff_instance *in)
{
	obs_properties_t *props = obs_properties_create();
	if (!in)
		return props;
	/* borrowed, not owned: the instance outlives every properties object built from it, so the
	   destroy callback stays NULL */
	obs_properties_set_param(props, in, NULL);
	refresh_packs(in); /* a pack installed from outside OBS shows up when the page is opened */

	obs_property_t *packs = obs_properties_add_list(props, S_PACK, obs_module_text("Foxfire.Pack"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	populate_pack_list(in, packs);
	obs_property_set_modified_callback(packs, on_pack_changed);

	obs_property_t *presets = obs_properties_add_list(props, S_PRESET, obs_module_text("Foxfire.Preset"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	populate_preset_list(in, presets, in->pack_id);
	obs_property_set_modified_callback(presets, on_preset_changed);

	if (!in->is_filter) {
		obs_properties_add_int(props, S_WIDTH, obs_module_text("Foxfire.Width"), 16, 8192, 1);
		obs_properties_add_int(props, S_HEIGHT, obs_module_text("Foxfire.Height"), 16, 8192, 1);
	}

	obs_property_t *mode = obs_properties_add_list(props, S_AUDIO_MODE, obs_module_text("Foxfire.Audio"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Foxfire.Audio.Master"), FF_AUDIO_MASTER);
	obs_property_list_add_int(mode, obs_module_text("Foxfire.Audio.Source"), FF_AUDIO_SOURCE);
	obs_property_set_modified_callback(mode, on_audio_mode_changed);

	obs_property_t *asrc = obs_properties_add_list(props, S_AUDIO_SOURCE,
						       obs_module_text("Foxfire.Audio.SourceName"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	populate_audio_sources(asrc);

	obs_properties_add_float_slider(props, S_GAIN, obs_module_text("Foxfire.Gain"), -24.0, 24.0, 0.1);
	obs_properties_add_float_slider(props, S_RELEASE, obs_module_text("Foxfire.Release"), 20.0, 2000.0, 5.0);
	obs_properties_add_float_slider(props, S_BEAT, obs_module_text("Foxfire.BeatSensitivity"), 0.1, 3.0, 0.05);

	obs_property_t *install = obs_properties_add_path(props, S_INSTALL, obs_module_text("Foxfire.InstallPack"),
							  OBS_PATH_FILE, "Pack (*.zip)", NULL);
	obs_property_set_modified_callback(install, on_install_changed);
	obs_properties_add_button2(props, "reload", obs_module_text("Foxfire.Reload"), on_reload, in);

	add_status_lines(in, props);
	add_licence_line(in, props);
	add_heavy_line(in, props);
	ff_renderer_add_properties(in->renderer, props); /* layer errors, then the preset's own knobs */
	return props;
}

gs_texture_t *ff_instance_render(struct ff_instance *in, gs_texture_t *input, uint32_t w, uint32_t h, float dt)
{
	if (!in)
		return NULL;
	/* a failed read can have torn the destination, so it lands in a local: the instance keeps
	   the last frame that was read whole */
	struct ff_frame f;
	if (ff_audio_read(in->audio, &f))
		in->frame = f;
	return ff_renderer_render(in->renderer, &in->frame, input, w, h, dt);
}
