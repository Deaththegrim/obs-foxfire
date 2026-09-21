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
#include "ff-alert-queue.h"
#include "ff-layers.h"
#include "ff-pack.h"
#include "ff-twitch.h"

#include <obs-module.h>
#include <pthread.h>
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

	/* The pack's art, drawn BEHIND the name. The same renderer the visualizer uses, so an
	   alert pack is a pack like any other -- same format, same licence gate, same packforge.
	   Its layers get `progress` (0..1 across the alert) so a pack animates its own entrance
	   instead of choosing from a fixed list of four transitions, which is what every
	   competitor offers. */
	struct ff_renderer *renderer;
	struct ff_pack_list packs;
	char pack_id[64];
	char loaded_preset[64]; /* what the renderer currently holds */
	char want_preset[64];   /* what the alert on screen asked for */
	bool art_dirty;         /* reload on the next render, where the graphics context is held */

	/* Per kind: a follow, a sub and a raid should not look and sound the same. The parity
	   study lists this twice -- as core event triggers and again as tier variations -- and it
	   is the difference between an alert system and a text box.

	   One renderer, reloaded when the preset actually changes, rather than one renderer per
	   kind. Seven renderers would each hold a ping/pong pair at the source's size, which at
	   1080p is over a hundred megabytes to avoid a shader compile that happens at the exact
	   moment an alert is fading in anyway. Consecutive alerts of one kind -- a raid, which is
	   the case that matters -- cost nothing either way. */
	struct {
		bool enabled;
		char template_[512];
		char preset[64];
		char sound[512];
	} kinds[FF_ALERT_KIND_COUNT];

	struct ff_alert_text style;
	char preset_fallback[64]; /* art used by any kind that has none of its own */
	char sound_fallback[512];
	char sound_path[512];     /* what the sound child currently holds */
	float duration; /* seconds an alert stays on screen */

	/* Pending alerts. Events will arrive on a network thread once the feed is real, and the
	   tick pops them on the video thread, so this is locked NOW rather than when the second
	   thread appears -- a queue that races is a queue that drops the alert nobody can reproduce. */
	struct ff_alert_queue queue;
	pthread_mutex_t qlock;
	/* The live feed. It owns a thread, so it is created last and destroyed FIRST -- its worker
	   calls ff_alert_enqueue, which touches the queue and the per-kind switches below. */
	struct ff_twitch *twitch;

	bool paused;                 /* stops STARTING new alerts; whatever is on screen still finishes */
	enum ff_alert_kind test_kind; /* which kind the test button fires */

	/* Playback state, written and read on the video thread only (tick and render both run
	   there), so it needs no lock of its own. */
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

/* Substitutes the message variables. Every value that came from the wire -- the name and the
   viewer's own message -- has already been through ff_alert_sanitise; this only joins strings.
   {amount} and {tier} are ours, so they are formatted here. */
static void render_template(const struct ff_alert_source *a, const struct ff_alert_event *e,
			    const char *safe_name, const char *safe_msg, char *out, size_t cap)
{
	char num[32], tier[16];
	snprintf(num, sizeof num, "%lld", (long long)e->amount);
	snprintf(tier, sizeof tier, "%d", e->tier);

	/* The kind's own message. There is deliberately NO shared message to fall back to: every
	   kind's default is a complete sentence ("{name} subscribed!", "{name} cheered {amount}
	   bits!"), so a shared box would never be consulted unless a streamer first blanked a
	   kind's -- a control that appears to work and does nothing. Art and sound DO have a
	   shared fallback, because those are genuinely often the same for every kind; the words
	   never are. */
	const char *tmpl = a->kinds[e->kind].template_;

	struct dstr t = {0};
	dstr_copy(&t, tmpl[0] ? tmpl : "{name}!");
	dstr_replace(&t, "{name}", safe_name ? safe_name : "");
	dstr_replace(&t, "{message}", safe_msg ? safe_msg : "");
	dstr_replace(&t, "{amount}", num);
	/* Prime and "not applicable" are both tier 0, and "Tier 0" is not a thing anyone says. */
	dstr_replace(&t, "{tier}", e->tier > 0 ? tier : "");
	dstr_replace(&t, "{kind}", ff_alert_kind_id(e->kind));
	snprintf(out, cap, "%s", t.array ? t.array : "");
	dstr_free(&t);
}

/* Starts one event NOW. Video thread only -- it touches the children and the playback state. */
static void start_alert(struct ff_alert_source *a, const struct ff_alert_event *e)
{
	char safe[192], safe_msg[320];
	size_t dropped = ff_alert_sanitise(e->name, safe, sizeof safe);
	dropped += ff_alert_sanitise(e->message, safe_msg, sizeof safe_msg);
	if (dropped)
		obs_log(LOG_INFO, "alerts: removed %zu unsafe character(s) from a name before drawing it",
			dropped);

	char body[512];
	render_template(a, e, safe, safe_msg, body, sizeof body);
	snprintf(a->showing, sizeof a->showing, "%s", body);

	ensure_text_child(a);
	if (a->text) {
		struct ff_alert_text t = a->style;
		snprintf(t.body, sizeof t.body, "%s", body);
		ff_alert_text_apply(a->text, &t);
	}

	/* the kind's art, then the kind's sound, each falling back to the shared setting */
	const char *preset = a->kinds[e->kind].preset[0] ? a->kinds[e->kind].preset : a->preset_fallback;
	if (strcmp(preset, a->want_preset)) {
		snprintf(a->want_preset, sizeof a->want_preset, "%s", preset);
		a->art_dirty = true;
	}
	const char *snd = a->kinds[e->kind].sound[0] ? a->kinds[e->kind].sound : a->sound_fallback;
	if (strcmp(snd, a->sound_path)) {
		snprintf(a->sound_path, sizeof a->sound_path, "%s", snd);
		ensure_sound_child(a);
	}

	ensure_sound_child(a);
	if (a->sound)
		obs_source_media_restart(a->sound); /* from the top, every time */

	a->elapsed = 0.f;
	a->playing = true;
	obs_log(LOG_INFO, "alerts: firing '%s'", body);
}

void ff_alert_enqueue(void *data, const struct ff_alert_event *e)
{
	struct ff_alert_source *a = data;
	if (!a || !e)
		return;
	if (e->kind < 0 || e->kind >= FF_ALERT_KIND_COUNT) {
		obs_log(LOG_WARNING, "alerts: ignoring an event of unknown kind %d", (int)e->kind);
		return;
	}
	if (!a->kinds[e->kind].enabled) {
		/* Turned off deliberately, so this is not a warning -- but it IS logged, because
		   "my follow alerts stopped working" and "I turned follow alerts off" look identical
		   from the outside and the log is the only place they differ. */
		obs_log(LOG_INFO, "alerts: '%s' alerts are switched off; ignoring one for '%s'",
			ff_alert_kind_id(e->kind), e->name);
		return;
	}
	pthread_mutex_lock(&a->qlock);
	bool ok = ff_alert_queue_push(&a->queue, e);
	uint64_t dropped = a->queue.dropped;
	size_t depth = a->queue.count;
	pthread_mutex_unlock(&a->qlock);
	if (!ok)
		/* Never silent. A dropped alert is a supporter who was never thanked, and the only
		   symptom on screen is an alert that simply did not happen. */
		obs_log(LOG_WARNING,
			"alerts: the queue is full (%d waiting) -- dropped an alert for '%s'; %llu "
			"dropped so far",
			FF_ALERT_QUEUE_MAX, e->name, (unsigned long long)dropped);
	else if (depth > 1)
		obs_log(LOG_INFO, "alerts: queued an alert for '%s' (%zu waiting)", e->name, depth);
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
	const char *pk = obs_data_get_string(s, "pack");
	if (strcmp(pk, a->pack_id)) {
		snprintf(a->pack_id, sizeof a->pack_id, "%s", pk);
		/* Deferred: loading an effect needs the graphics context, and update() is called
		   from the UI thread. The visualizer enters the context here; this source does not
		   have to, because it already has a per-frame hook that is inside it. */
		a->art_dirty = true;
	}
	snprintf(a->preset_fallback, sizeof a->preset_fallback, "%s", obs_data_get_string(s, "preset"));
	snprintf(a->sound_fallback, sizeof a->sound_fallback, "%s", obs_data_get_string(s, "sound"));
	for (int k = 0; k < FF_ALERT_KIND_COUNT; k++) {
		char key[96];
		const char *id = ff_alert_kind_id((enum ff_alert_kind)k);
		snprintf(key, sizeof key, "k.%s.enabled", id);
		a->kinds[k].enabled = obs_data_get_bool(s, key);
		snprintf(key, sizeof key, "k.%s.template", id);
		snprintf(a->kinds[k].template_, sizeof a->kinds[k].template_, "%s",
			 obs_data_get_string(s, key));
		snprintf(key, sizeof key, "k.%s.preset", id);
		snprintf(a->kinds[k].preset, sizeof a->kinds[k].preset, "%s", obs_data_get_string(s, key));
		snprintf(key, sizeof key, "k.%s.sound", id);
		snprintf(a->kinds[k].sound, sizeof a->kinds[k].sound, "%s", obs_data_get_string(s, key));
	}
	a->width = (uint32_t)obs_data_get_int(s, "width");
	a->height = (uint32_t)obs_data_get_int(s, "height");
	snprintf(a->style.face, sizeof a->style.face, "%s", obs_data_get_string(s, "font_face"));
	a->style.size = (int)obs_data_get_int(s, "font_size");
	a->style.colour = (uint32_t)obs_data_get_int(s, "colour");
	a->style.bold = obs_data_get_bool(s, "bold");
	a->style.outline = obs_data_get_bool(s, "outline");
	a->style.shadow = obs_data_get_bool(s, "shadow");
	a->duration = (float)obs_data_get_double(s, "duration");
	a->paused = obs_data_get_bool(s, "paused");
	enum ff_alert_kind tk;
	/* an unrecognised id leaves the previous choice alone rather than snapping to follow --
	   ff_alert_kind_parse refuses instead of defaulting for exactly this reason */
	if (ff_alert_kind_parse(obs_data_get_string(s, "test_kind"), &tk))
		a->test_kind = tk;
	snprintf(a->sound_path, sizeof a->sound_path, "%s", obs_data_get_string(s, "sound"));
	ensure_sound_child(a);
	if (a->twitch) {
		ff_twitch_set_client_id(a->twitch, obs_data_get_string(s, "twitch_client_id"));
		ff_twitch_set_enabled(a->twitch, obs_data_get_bool(s, "twitch_enabled"));
	}
}

static void *alert_create(obs_data_t *s, obs_source_t *self)
{
	struct ff_alert_source *a = bzalloc(sizeof *a);
	a->self = self;
	pthread_mutex_init(&a->qlock, NULL);
	ff_alert_queue_init(&a->queue);
	obs_enter_graphics();
	a->renderer = ff_renderer_create();
	obs_leave_graphics();
	ff_packs_scan(&a->packs);
	alert_update(a, s);
	/* Last: the worker can call ff_alert_enqueue the moment it exists, and everything that
	   touches -- the queue, its lock, the per-kind switches -- has to be set up by then. */
	char *cfg = obs_module_config_path("packs");
	a->twitch = ff_twitch_create(ff_alert_enqueue, a, cfg);
	bfree(cfg);
	if (a->twitch) {
		ff_twitch_set_client_id(a->twitch, obs_data_get_string(s, "twitch_client_id"));
		ff_twitch_set_enabled(a->twitch, obs_data_get_bool(s, "twitch_enabled"));
	}
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
	pthread_mutex_destroy(&a->qlock);
	obs_enter_graphics();
	/* FIRST, and before anything it might touch: the worker thread calls ff_alert_enqueue,
	   which locks a->qlock and reads a->kinds. Destroying those out from under a running
	   thread is a crash that only happens when a stream ends while an alert is arriving. */
	ff_twitch_destroy(a->twitch);
	a->twitch = NULL;

	ff_renderer_destroy(a->renderer);
	obs_leave_graphics();
	ff_packs_free(&a->packs);
	bfree(a);
}

/* What each kind says by default. Every one is a complete sentence a streamer could leave alone,
   because a default of "" would draw an empty card for an event that fired correctly. */
static const struct {
	const char *id, *tmpl;
} KIND_DEFAULTS[FF_ALERT_KIND_COUNT] = {
	{"follow", "{name} followed!"},
	{"sub", "{name} subscribed!"},
	{"resub", "{name} resubscribed for {amount} months!"},
	{"gift", "{name} gifted {amount} subs!"},
	{"bits", "{name} cheered {amount} bits!"},
	{"raid", "{name} raided with {amount}!"},
	{"redeem", "{name} redeemed {message}"},
};

static void alert_defaults(obs_data_t *s)
{
	for (int k = 0; k < FF_ALERT_KIND_COUNT; k++) {
		char key[96];
		const char *id = ff_alert_kind_id((enum ff_alert_kind)k);
		snprintf(key, sizeof key, "k.%s.enabled", id);
		obs_data_set_default_bool(s, key, true); /* all on: a new install works out of the box */
		snprintf(key, sizeof key, "k.%s.template", id);
		/* looked up by id rather than by index, so a reordering of the enum cannot silently
		   give "raid" the bits message */
		const char *tmpl = "{name}!";
		for (int i = 0; i < FF_ALERT_KIND_COUNT; i++)
			if (KIND_DEFAULTS[i].id && !strcmp(KIND_DEFAULTS[i].id, id))
				tmpl = KIND_DEFAULTS[i].tmpl;
		obs_data_set_default_string(s, key, tmpl);
	}
	/* OFF by default. Connecting to someone's Twitch account is not something a source should
	   start doing because it was added to a scene. */
	obs_data_set_default_bool(s, "twitch_enabled", false);
	obs_data_set_default_int(s, "width", 800);
	obs_data_set_default_int(s, "height", 240);
	obs_data_set_default_string(s, "font_face", "Sans Serif");
	obs_data_set_default_int(s, "font_size", 48);
	obs_data_set_default_int(s, "colour", 0xFFFFFFFF);
	obs_data_set_default_bool(s, "outline", true);
	obs_data_set_default_bool(s, "shadow", true);
	obs_data_set_default_double(s, "duration", 5.0);
	obs_data_set_default_string(s, "test_kind", "follow");
}

static bool on_twitch_connect(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct ff_alert_source *a = data;
	if (a && a->twitch)
		ff_twitch_sign_in(a->twitch);
	/* true: rebuild the page so the code appears without the streamer having to close and
	   reopen properties. The code arrives on the worker thread a moment later, so the status
	   line is what actually carries it -- see add_twitch_status. */
	return true;
}

static bool on_twitch_sign_out(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct ff_alert_source *a = data;
	if (a && a->twitch)
		ff_twitch_sign_out(a->twitch);
	return true;
}

/* The connection's state, as a line the streamer can act on.
 *
 * This is the whole user interface of the feed, so it says what to DO, not what happened: the
 * code to type while signing in, which alert types were refused, and that a sign-in has expired
 * rather than "error 401". The alternative is a support message that says "it stopped working". */
static void add_twitch_status(struct ff_alert_source *a, obs_properties_t *p)
{
	obs_properties_t *g = obs_properties_create();
	obs_properties_add_text(g, "twitch_client_id", obs_module_text("Foxfire.Twitch.ClientId"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_bool(g, "twitch_enabled", obs_module_text("Foxfire.Twitch.Enabled"));
	obs_properties_add_button2(g, "twitch_connect", obs_module_text("Foxfire.Twitch.Connect"),
				   on_twitch_connect, a);
	obs_properties_add_button2(g, "twitch_signout", obs_module_text("Foxfire.Twitch.SignOut"),
				   on_twitch_sign_out, a);

	char line[512] = {0}, code[32] = {0}, url[256] = {0};
	enum ff_twitch_state st = ff_twitch_status(a ? a->twitch : NULL, line, sizeof line, code,
						   sizeof code, url, sizeof url);
	obs_property_t *info = obs_properties_add_text(g, "twitch_status", line, OBS_TEXT_INFO);
	/* An error has to LOOK like one. A red line is the difference between a streamer noticing
	   their sign-in expired and finding out from a viewer asking why nobody got thanked. */
	obs_property_text_set_info_type(info,
					st == FF_TWS_FAILED ? OBS_TEXT_INFO_ERROR
							    : (st == FF_TWS_RETRYING
								       ? OBS_TEXT_INFO_WARNING
								       : OBS_TEXT_INFO_NORMAL));
	obs_properties_add_group(p, "twitch", obs_module_text("Foxfire.Twitch.Group"),
				 OBS_GROUP_NORMAL, g);
}

static bool on_test_fire(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	/* A name with a right-to-left override and a newline in it, on purpose. The test button is
	   the one place a viewer can see what the sanitiser does, and a button that fires a tidy
	   name proves only that tidy names work. */
	struct ff_alert_source *a = data;
	struct ff_alert_event e = {0};
	e.kind = a->test_kind;
	/* plausible numbers, so {amount} and {tier} in a template are visible in the test rather
	   than rendering as "0" and sending someone looking for a bug */
	e.amount = 42;
	e.tier = 1;
	snprintf(e.message, sizeof e.message, "a message from the test button");
	snprintf(e.name, sizeof e.name, "Test\xe2\x80\xaeViewer\n42");
	ff_alert_enqueue(data, &e);
	return false;
}

static bool on_skip(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct ff_alert_source *a = data;
	/* Ends the one on screen. The next tick starts whatever is next, so skipping through a
	   raid is just pressing this repeatedly -- which is what a streamer actually does. */
	a->elapsed = a->duration;
	return false;
}

static bool on_clear(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct ff_alert_source *a = data;
	pthread_mutex_lock(&a->qlock);
	size_t n = ff_alert_queue_clear(&a->queue);
	pthread_mutex_unlock(&a->qlock);
	obs_log(LOG_INFO, "alerts: cleared %zu queued alert(s)", n);
	return false;
}

static obs_properties_t *alert_props(void *d)
{
	UNUSED_PARAMETER(d);
	struct ff_alert_source *a = d;
	obs_properties_t *p = obs_properties_create();

	obs_property_t *packs = obs_properties_add_list(p, "pack", obs_module_text("Foxfire.Pack"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(packs, obs_module_text("Foxfire.Alert.NoArt"), "");
	obs_property_t *presets = obs_properties_add_list(p, "preset", obs_module_text("Foxfire.Preset"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(presets, obs_module_text("Foxfire.Alert.NoArt"), "");
	if (a) {
		for (size_t i = 0; i < a->packs.n; i++) {
			const struct ff_pack *pack = &a->packs.packs[i];
			/* only packs that actually contain an alert preset: offering one that has
			   none gives a viewer a choice that can only disappoint */
			bool any = false;
			for (size_t j = 0; j < pack->npresets; j++)
				if (!strcmp(pack->presets[j].kind, "alert"))
					any = true;
			if (any)
				obs_property_list_add_string(packs, pack->name, pack->id);
		}
		const struct ff_pack *chosen = ff_packs_find(&a->packs, a->pack_id);
		if (chosen)
			for (size_t j = 0; j < chosen->npresets; j++)
				if (!strcmp(chosen->presets[j].kind, "alert"))
					obs_property_list_add_string(presets, chosen->presets[j].name,
								     chosen->presets[j].id);
	}

	obs_properties_add_int(p, "width", obs_module_text("Foxfire.Width"), 16, 8192, 1);
	obs_properties_add_int(p, "height", obs_module_text("Foxfire.Height"), 16, 8192, 1);
	obs_properties_add_font(p, "font_face", obs_module_text("Foxfire.Alert.Font"));
	obs_properties_add_int_slider(p, "font_size", obs_module_text("Foxfire.Alert.FontSize"), 8, 256, 1);
	obs_properties_add_color_alpha(p, "colour", obs_module_text("Foxfire.Alert.Colour"));
	obs_properties_add_bool(p, "outline", obs_module_text("Foxfire.Alert.Outline"));
	obs_properties_add_bool(p, "shadow", obs_module_text("Foxfire.Alert.Shadow"));
	obs_properties_add_path(p, "sound", obs_module_text("Foxfire.Alert.Sound"), OBS_PATH_FILE,
				"Audio (*.wav *.mp3 *.ogg *.flac *.m4a);;All files (*.*)", NULL);
	obs_properties_add_float_slider(p, "duration", obs_module_text("Foxfire.Alert.Duration"), 1.0, 30.0,
					0.1);
	/* One group per kind, each collapsed into its own box, so seven kinds x four settings does
	   not become a wall. The shared Message/Preset/Sound above stay as the fallback for any
	   kind that has none of its own -- so a streamer who wants one look everywhere sets it
	   once and never opens these. */
	for (int k = 0; k < FF_ALERT_KIND_COUNT; k++) {
		const char *id = ff_alert_kind_id((enum ff_alert_kind)k);
		char key[96], label[96];
		obs_properties_t *g = obs_properties_create();

		snprintf(key, sizeof key, "k.%s.enabled", id);
		obs_properties_add_bool(g, key, obs_module_text("Foxfire.Alert.KindEnabled"));
		snprintf(key, sizeof key, "k.%s.template", id);
		obs_properties_add_text(g, key, obs_module_text("Foxfire.Alert.Template"), OBS_TEXT_DEFAULT);

		snprintf(key, sizeof key, "k.%s.preset", id);
		obs_property_t *pl = obs_properties_add_list(g, key, obs_module_text("Foxfire.Preset"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(pl, obs_module_text("Foxfire.Alert.SameAsAbove"), "");
		if (a) {
			const struct ff_pack *chosen = ff_packs_find(&a->packs, a->pack_id);
			if (chosen)
				for (size_t j = 0; j < chosen->npresets; j++)
					if (!strcmp(chosen->presets[j].kind, "alert"))
						obs_property_list_add_string(pl, chosen->presets[j].name,
									     chosen->presets[j].id);
		}
		snprintf(key, sizeof key, "k.%s.sound", id);
		obs_properties_add_path(g, key, obs_module_text("Foxfire.Alert.Sound"), OBS_PATH_FILE,
					"Audio (*.wav *.mp3 *.ogg *.flac *.m4a);;All files (*.*)", NULL);

		snprintf(key, sizeof key, "k.%s", id);
		snprintf(label, sizeof label, "Foxfire.Alert.Kind.%s", id);
		obs_properties_add_group(p, key, obs_module_text(label), OBS_GROUP_NORMAL, g);
	}

	add_twitch_status(a, p);

	obs_properties_add_bool(p, "paused", obs_module_text("Foxfire.Alert.Paused"));
	obs_property_t *tk = obs_properties_add_list(p, "test_kind", obs_module_text("Foxfire.Alert.TestKind"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (int k = 0; k < FF_ALERT_KIND_COUNT; k++) {
		const char *id = ff_alert_kind_id((enum ff_alert_kind)k);
		char lk[96];
		snprintf(lk, sizeof lk, "Foxfire.Alert.Kind.%s", id);
		obs_property_list_add_string(tk, obs_module_text(lk), id);
	}
	obs_properties_add_button2(p, "test", obs_module_text("Foxfire.Alert.Test"), on_test_fire, d);
	obs_properties_add_button2(p, "skip", obs_module_text("Foxfire.Alert.Skip"), on_skip, d);
	obs_properties_add_button2(p, "clear", obs_module_text("Foxfire.Alert.Clear"), on_clear, d);
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
	if (a->playing) {
		a->elapsed += dt;
		if (a->elapsed < a->duration)
			return;
		a->playing = false;
		a->elapsed = 0.f;
		/* The sound stops with the picture. Skip has to silence the alert it skipped -- a
		   streamer pressing Skip during a raid expects the noise to go too, and without this
		   the sound of an alert nobody can see keeps playing over the next one. It also caps
		   a sound longer than the duration, which is what setting a duration means; the
		   stream kit's own clips are 6.12s against a 5s default. */
		if (a->sound)
			obs_source_media_stop(a->sound);
	}
	if (a->paused)
		return; /* pause stops STARTING alerts; the queue keeps filling and nothing is lost */

	struct ff_alert_event next;
	pthread_mutex_lock(&a->qlock);
	bool have = ff_alert_queue_pop(&a->queue, &next);
	pthread_mutex_unlock(&a->qlock);
	if (have)
		start_alert(a, &next);
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
	if (a->art_dirty) {
		/* here, not in update(): this callback already runs inside the graphics context,
		   and ff_renderer_load requires it */
		const struct ff_pack *pack = ff_packs_find(&a->packs, a->pack_id);
		const struct ff_preset *preset = ff_pack_find_preset(pack, a->want_preset);
		if (preset && strcmp(preset->kind, "alert")) {
			obs_log(LOG_WARNING,
				"alerts: preset '%s' of pack '%s' is kind '%s', not 'alert'; not loading it",
				a->want_preset, a->pack_id, preset->kind);
			preset = NULL;
		}
		ff_renderer_load(a->renderer, pack, preset);
		snprintf(a->loaded_preset, sizeof a->loaded_preset, "%s", preset ? preset->id : "");
		a->art_dirty = false;
	}
	if (!a->playing)
		return; /* idle draws NOTHING: an alert overlay is idle almost all of the time */

	float progress = a->duration > 0.f ? a->elapsed / a->duration : 0.f;
	if (progress > 1.f)
		progress = 1.f;

	/* the pack's art first, underneath */
	if (a->renderer) {
		gs_texture_t *art = ff_renderer_render(a->renderer, NULL, progress, NULL, a->width,
						       a->height, 1.f / 60.f);
		if (art) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
			gs_effect_set_texture(img, art);
			while (gs_effect_loop(def, "Draw"))
				gs_draw_sprite(art, 0, a->width, a->height);
		}
	}

	if (!a->text)
		return;
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
