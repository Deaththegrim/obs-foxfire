/*
Foxfire Alerts
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

/* The alert source: a composite that owns a text child and a sound child.
 *
 * It is IDLE almost all of the time and draws nothing at all when it is -- an alert overlay
 * spends its life waiting. When one fires it draws for `duration` seconds and goes quiet again.
 *
 * Two children, both private (in no scene, owned entirely by this source):
 *   - a text source, whose KIND is resolved at runtime (see ff-alert.c) because the name differs
 *     per platform, and which the engine feeds through its own vocabulary rather than letting a
 *     pack name that source's settings keys;
 *   - an ffmpeg_source for the sound, because it already decodes whatever a streamer drops in.
 *
 * A private source sits in no scene, so nothing mixes its audio. That is what OBS_SOURCE_COMPOSITE
 * and audio_render are for: libobs asks us for audio, and we hand it the sound child's mix. The
 * enum_active_sources callback is how OBS learns the children exist -- without it the tree is
 * invisible to it and the children never tick.
 */

#include "ff-alert.h"
#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/graphics.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <string.h>

#define FF_ALERT_TEXT_CHILD "foxfire alert text"
#define FF_ALERT_SOUND_CHILD "foxfire alert sound"

struct ff_alert_source {
	obs_source_t *self;
	obs_source_t *text;  /* private child, NULL if this OBS has no text source we can drive */
	obs_source_t *sound; /* private child, NULL until a sound file is chosen */

	uint32_t width, height;
	struct ff_alert_text style;
	char template_[512]; /* the message, with {name} standing in for the sender */
	char sound_path[512];
	float duration; /* seconds an alert stays on screen */

	/* Playback state. `elapsed` is the only thing the video thread writes and the only thing
	   the tick thread reads, and both are floats updated once per frame -- a torn read costs
	   at most one frame of fade, so this deliberately takes no lock. */
	bool playing;
	float elapsed;
	char showing[512]; /* the sanitised text currently on screen, for the log */
};

/* ------------------------------------------------------------------ children */

static void ensure_text_child(struct ff_alert_source *a)
{
	if (a->text)
		return;
	const char *kind = ff_alert_text_kind();
	if (!kind)
		return; /* ff_alert_text_kind() already said so, once, with the list it tried */
	obs_data_t *s = obs_data_create();
	a->text = obs_source_create_private(kind, FF_ALERT_TEXT_CHILD, s);
	obs_data_release(s);
	if (!a->text) {
		obs_log(LOG_WARNING, "alerts: '%s' is registered but would not create; no name will be drawn",
			kind);
		return;
	}
	obs_source_add_active_child(a->self, a->text);
}

static void ensure_sound_child(struct ff_alert_source *a)
{
	if (!a->sound_path[0]) {
		if (a->sound) {
			obs_source_remove_active_child(a->self, a->sound);
			obs_source_release(a->sound);
			a->sound = NULL;
		}
		return;
	}
	if (!a->sound) {
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "is_local_file", true);
		obs_data_set_bool(s, "looping", false);
		/* restart_on_activate would replay the sound every time the SCENE became active,
		   which for an alert is a sound firing when nobody triggered anything */
		obs_data_set_bool(s, "restart_on_activate", false);
		obs_data_set_bool(s, "clear_on_media_end", true);
		obs_data_set_string(s, "local_file", a->sound_path);
		a->sound = obs_source_create_private("ffmpeg_source", FF_ALERT_SOUND_CHILD, s);
		obs_data_release(s);
		if (!a->sound) {
			obs_log(LOG_WARNING, "alerts: could not create a media source for '%s'",
				a->sound_path);
			return;
		}
		obs_source_add_active_child(a->self, a->sound);
		return;
	}
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_string(s, "local_file", a->sound_path);
	obs_source_update(a->sound, s);
	obs_data_release(s);
}

/* --------------------------------------------------------------------- fire */

/* Substitutes {name} in the template. Everything the caller supplies has already been through
   ff_alert_sanitise -- this only joins strings. */
static void render_template(const struct ff_alert_source *a, const char *name, char *out, size_t cap)
{
	struct dstr t = {0};
	dstr_copy(&t, a->template_[0] ? a->template_ : "{name} followed!");
	dstr_replace(&t, "{name}", name ? name : "");
	snprintf(out, cap, "%s", t.array ? t.array : "");
	dstr_free(&t);
}

void ff_alert_fire(void *data, const char *raw_name)
{
	struct ff_alert_source *a = data;
	char safe[192];
	size_t dropped = ff_alert_sanitise(raw_name, safe, sizeof safe);
	if (dropped)
		obs_log(LOG_INFO, "alerts: removed %zu unsafe character(s) from a name before drawing it",
			dropped);

	char body[512];
	render_template(a, safe, body, sizeof body);
	snprintf(a->showing, sizeof a->showing, "%s", body);

	ensure_text_child(a);
	if (a->text) {
		struct ff_alert_text t = a->style;
		snprintf(t.body, sizeof t.body, "%s", body);
		ff_alert_text_apply(a->text, &t);
	}

	ensure_sound_child(a);
	if (a->sound)
		obs_source_media_restart(a->sound); /* from the top, every time */

	a->elapsed = 0.f;
	a->playing = true;
	obs_log(LOG_INFO, "alerts: firing '%s'", body);
}

/* ---------------------------------------------------------------- obs source */

static const char *alert_name(void *d)
{
	UNUSED_PARAMETER(d);
	return obs_module_text("Foxfire.Alert");
}

static void alert_update(void *d, obs_data_t *s)
{
	struct ff_alert_source *a = d;
	a->width = (uint32_t)obs_data_get_int(s, "width");
	a->height = (uint32_t)obs_data_get_int(s, "height");
	snprintf(a->template_, sizeof a->template_, "%s", obs_data_get_string(s, "template"));
	snprintf(a->style.face, sizeof a->style.face, "%s", obs_data_get_string(s, "font_face"));
	a->style.size = (int)obs_data_get_int(s, "font_size");
	a->style.colour = (uint32_t)obs_data_get_int(s, "colour");
	a->style.bold = obs_data_get_bool(s, "bold");
	a->style.outline = obs_data_get_bool(s, "outline");
	a->style.shadow = obs_data_get_bool(s, "shadow");
	a->duration = (float)obs_data_get_double(s, "duration");
	snprintf(a->sound_path, sizeof a->sound_path, "%s", obs_data_get_string(s, "sound"));
	ensure_sound_child(a);
}

static void *alert_create(obs_data_t *s, obs_source_t *self)
{
	struct ff_alert_source *a = bzalloc(sizeof *a);
	a->self = self;
	alert_update(a, s);
	return a;
}

static void alert_destroy(void *d)
{
	struct ff_alert_source *a = d;
	if (a->text) {
		obs_source_remove_active_child(a->self, a->text);
		obs_source_release(a->text);
	}
	if (a->sound) {
		obs_source_remove_active_child(a->self, a->sound);
		obs_source_release(a->sound);
	}
	bfree(a);
}

static void alert_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "width", 800);
	obs_data_set_default_int(s, "height", 240);
	obs_data_set_default_string(s, "template", "{name} followed!");
	obs_data_set_default_string(s, "font_face", "Sans Serif");
	obs_data_set_default_int(s, "font_size", 48);
	obs_data_set_default_int(s, "colour", 0xFFFFFFFF);
	obs_data_set_default_bool(s, "outline", true);
	obs_data_set_default_bool(s, "shadow", true);
	obs_data_set_default_double(s, "duration", 5.0);
}

static bool on_test_fire(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	/* A name with a right-to-left override and a newline in it, on purpose. The test button is
	   the one place a viewer can see what the sanitiser does, and a button that fires a tidy
	   name proves only that tidy names work. */
	ff_alert_fire(data, "Test\xe2\x80\xaeViewer\n42");
	return false;
}

static obs_properties_t *alert_props(void *d)
{
	UNUSED_PARAMETER(d);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int(p, "width", obs_module_text("Foxfire.Width"), 16, 8192, 1);
	obs_properties_add_int(p, "height", obs_module_text("Foxfire.Height"), 16, 8192, 1);
	obs_properties_add_text(p, "template", obs_module_text("Foxfire.Alert.Template"), OBS_TEXT_DEFAULT);
	obs_properties_add_font(p, "font_face", obs_module_text("Foxfire.Alert.Font"));
	obs_properties_add_int_slider(p, "font_size", obs_module_text("Foxfire.Alert.FontSize"), 8, 256, 1);
	obs_properties_add_color_alpha(p, "colour", obs_module_text("Foxfire.Alert.Colour"));
	obs_properties_add_bool(p, "outline", obs_module_text("Foxfire.Alert.Outline"));
	obs_properties_add_bool(p, "shadow", obs_module_text("Foxfire.Alert.Shadow"));
	obs_properties_add_path(p, "sound", obs_module_text("Foxfire.Alert.Sound"), OBS_PATH_FILE,
				"Audio (*.wav *.mp3 *.ogg *.flac *.m4a);;All files (*.*)", NULL);
	obs_properties_add_float_slider(p, "duration", obs_module_text("Foxfire.Alert.Duration"), 1.0, 30.0,
					0.1);
	obs_properties_add_button2(p, "test", obs_module_text("Foxfire.Alert.Test"), on_test_fire, d);
	if (!ff_alert_text_kind())
		obs_properties_add_text(p, "notext", obs_module_text("Foxfire.Alert.NoTextSource"),
					OBS_TEXT_INFO);
	return p;
}

static uint32_t alert_w(void *d)
{
	return ((struct ff_alert_source *)d)->width;
}

static uint32_t alert_h(void *d)
{
	return ((struct ff_alert_source *)d)->height;
}

static void alert_tick(void *d, float dt)
{
	struct ff_alert_source *a = d;
	if (!a->playing)
		return;
	a->elapsed += dt;
	if (a->elapsed >= a->duration) {
		a->playing = false;
		a->elapsed = 0.f;
	}
}

static void alert_enum(void *d, obs_source_enum_proc_t cb, void *param)
{
	struct ff_alert_source *a = d;
	/* Both children, ALWAYS -- not only while an alert is playing. OBS uses this to build the
	   active tree, and a child that disappears from it between frames is one whose audio stops
	   being mixed halfway through its own sound. */
	if (a->text)
		cb(a->self, a->text, param);
	if (a->sound)
		cb(a->self, a->sound, param);
}

static void alert_render(void *d, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct ff_alert_source *a = d;
	if (!a->playing || !a->text)
		return; /* idle draws NOTHING: an alert overlay is idle almost all of the time */

	uint32_t tw = obs_source_get_width(a->text), th = obs_source_get_height(a->text);
	if (!tw || !th)
		return;

	/* centred, and nudged by a short rise on the way in and out. Kept to alpha rather than
	   movement so a nameplate behind it does not appear to slide. */
	float fade = 1.f;
	const float f = 0.25f;
	if (a->elapsed < f)
		fade = a->elapsed / f;
	else if (a->duration - a->elapsed < f)
		fade = (a->duration - a->elapsed) / f;
	if (fade < 0.f)
		fade = 0.f;
	if (fade > 1.f)
		fade = 1.f;

	gs_matrix_push();
	gs_matrix_translate3f((float)((int)a->width - (int)tw) * 0.5f,
			      (float)((int)a->height - (int)th) * 0.5f, 0.f);
	gs_blend_state_push();
	gs_reset_blend_state();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *mul = gs_effect_get_param_by_name(def, "color");
	if (mul) {
		struct vec4 c;
		vec4_set(&c, 1.f, 1.f, 1.f, fade);
		gs_effect_set_vec4(mul, &c);
	}
	obs_source_video_render(a->text);
	gs_blend_state_pop();
	gs_matrix_pop();
}

static bool alert_audio_render(void *d, uint64_t *ts_out, struct obs_source_audio_mix *out,
			       uint32_t mixers, size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(sample_rate);
	struct ff_alert_source *a = d;
	if (!a->sound)
		return false;
	uint64_t ts = obs_source_get_audio_timestamp(a->sound);
	if (!ts)
		return false; /* the child has produced no audio yet; say so rather than emitting silence */

	struct obs_source_audio_mix child;
	obs_source_get_audio_mix(a->sound, &child);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1u << mix)) == 0)
			continue;
		for (size_t ch = 0; ch < channels; ch++)
			memcpy(out->output[mix].data[ch], child.output[mix].data[ch],
			       AUDIO_OUTPUT_FRAMES * sizeof(float));
	}
	*ts_out = ts;
	return true;
}

struct obs_source_info ff_alert_source_info = {
	.id = "foxfire_alert",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* COMPOSITE, and deliberately NOT OBS_SOURCE_AUDIO. Adding it looks right -- the source
	   does emit sound -- and libobs refuses the registration outright:
	     "obs_register_source: Source 'foxfire_alert': Composite sources cannot be audio sources"
	   and the kind then does not exist at all. For a composite, audio_render IS the audio path;
	   OBS_SOURCE_AUDIO is for a source that hands libobs raw PCM through obs_source_output_audio.
	   One consequence worth knowing: obs-websocket gates its volume meters and
	   GetInputAudioTracks on OBS_SOURCE_AUDIO, so it reports "this input does not support audio"
	   (code 604) for this source even though the sound reaches the recording perfectly well.
	   That is why tools/alert-proof.py measures a RECORDING rather than a meter. */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE,
	.get_name = alert_name,
	.create = alert_create,
	.destroy = alert_destroy,
	.update = alert_update,
	.get_defaults = alert_defaults,
	.get_properties = alert_props,
	.get_width = alert_w,
	.get_height = alert_h,
	.video_tick = alert_tick,
	.video_render = alert_render,
	.enum_active_sources = alert_enum,
	.audio_render = alert_audio_render,
	.icon_type = OBS_ICON_TYPE_TEXT,
};
