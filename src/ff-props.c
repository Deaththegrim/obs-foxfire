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

/* Threading contract
   ------------------
   Two threads reach an ff_instance and neither waits on the other:

     VIDEO thread   ff_instance_update  -- libobs defers a video source's update
                    (obs_source_update only bumps defer_update_count; obs_source_video_tick runs
                    the callback), so update() executes here, NOT on whichever thread asked for it
                    ff_instance_render  -- from video_render, already inside the graphics context

     UI / RPC       ff_instance_properties and every modified/clicked callback below. Both the Qt
                    properties dialog and obs-websocket land here.

     Either         ff_instance_create / ff_instance_destroy, which libobs serialises against the
                    video thread's use of the source.

   state_lock serialises every field the two threads share: packs, status, install_msg,
   install_failed, reload_pending, initialised, pack_id/preset_id, and the renderer's layer array
   (ff_renderer_load frees layers[].params on the video thread while ff_renderer_add_properties
   walks them on the UI one). Two rules keep it from becoming a stall:

     - the pack rescan is slow (readdir, a JSON parse and an Ed25519 verify per pack) and runs
       WITHOUT the lock into a fresh list, which is swapped in under the lock and the displaced
       list freed after -- see refresh_packs();
     - the lock IS held across ff_renderer_load + ff_renderer_apply_settings. There is no correct
       way to let the UI walk a layer array that is being freed, so the properties thread waits
       out a preset load. It is NOT unconditionally quick any more: apply_settings decodes every
       bound image from disk, so a viewer who picks a large PNG stalls the properties thread for
       as long as that decode takes. Acceptable because it is bounded by one file and happens on
       an explicit user action, but do not add anything slower under this lock without moving
       the work out the way refresh_packs() does.

   Lock order is state_lock -> graphics, and nothing takes state_lock while holding the graphics
   context (ff_instance_render does not touch it). ff_audio's conn_lock and libobs's sources_mutex
   are taken outside state_lock, never under it.

   Not covered, deliberately: in->frame is touched only by the video thread, and in->width/height
   are a pair of aligned uint32_t written on that same thread -- by update() for a source, or by
   video_render (from the target's own size) for a filter -- and read by get_width/get_height; a
   lock there would buy nothing a torn read could not already rule out. in->dt is the same story:
   only video_tick writes it and only video_render (via ff_instance_render) reads it. The guarantee
   is weaker than "tick then render for this instance back to back" -- libobs runs every source's
   video_tick for a frame before it runs any source's video_render for that frame, all on the one
   video thread -- but it is enough: by the time this instance's video_render reads in->dt, this
   frame's video_tick has already written it, and nothing else touches the field in between.

   Not covered, and NOT ours to fix: the obs_data_t settings object is mutated by both threads --
   the UI clears S_INSTALL in on_install_changed, the video thread walks the item hash in
   erase_layer_keys and inserts defaults in ff_renderer_set_defaults. state_lock does not guard it,
   because the same object is written concurrently by libobs itself (obs_source_update applies into
   source->context.settings from whichever thread calls it) and by every other plugin that clears a
   setting from a modified callback. It is pre-existing and endemic; taking our lock around our own
   accesses would not make it safe, only look safe. Flagged here so the sentence above is read as
   "every field of this struct", not "every byte the two threads touch". */

#include "ff-props.h"
#include "ff-dock-proto.h"
#include <plugin-support.h>
#include "ff-compat.h"
#include <util/darray.h>
#include <util/dstr.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ helpers */

/* Substitutes "%1" and "%2" in a localised string. The locale file is the only place the
   sentences live, so every caller goes through here rather than composing prose in C. */
static void fmt_text(char *out, size_t cap, const char *key, const char *a, const char *b)
{
	struct dstr t = {0};
	dstr_copy(&t, obs_module_text(key));
	if (a)
		dstr_replace(&t, "%1", a);
	if (b)
		dstr_replace(&t, "%2", b);
	snprintf(out, cap, "%s", t.array ? t.array : "");
	dstr_free(&t);
}

/* Local time, not UTC: this date is the entitlement cut-off the user compares against their own
   calendar, and it reading a day early in Australia would be a support ticket. */
static void fmt_date(int64_t t, char *out, size_t cap)
{
	time_t tt = (time_t)t;
	struct tm tm;
	if (!ff_localtime(&tt, &tm) || strftime(out, cap, "%Y-%m-%d", &tm) == 0)
		snprintf(out, cap, "?");
}

/* Scans off-lock into a fresh list, swaps it in under the lock, frees the displaced one after.
   Doing it in place would free packs out from under the video thread, which holds borrowed
   `struct ff_pack *` pointers for the length of a load. Never call this with state_lock held. */
static void refresh_packs(struct ff_instance *in)
{
	struct ff_pack_list fresh;
	memset(&fresh, 0, sizeof fresh);
	ff_packs_scan(&fresh);

	pthread_mutex_lock(&in->state_lock);
	struct ff_pack_list displaced = in->packs;
	in->packs = fresh;
	pthread_mutex_unlock(&in->state_lock);

	ff_packs_free(&displaced);
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
		fmt_date(pk->licence.entitled_through, date, sizeof date);
		dstr_copy(&t, obs_module_text("Foxfire.Licence.OK"));
		dstr_replace(&t, "%1", pk->licensee_name[0] ? pk->licensee_name : pk->licence.discord_id);
		dstr_replace(&t, "%2", date);
		break;
	case FF_LIC_NEWER:
		/* no date interpolated: naming the day their sub lapsed adds nothing the sentence does
		   not already say, and reads as a reprimand */
		dstr_copy(&t, obs_module_text("Foxfire.Licence.Newer"));
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

/* A pack someone is not entitled to loads ZERO layers rather than degrading to shader defaults.
   NEWER means the pack post-dates what they paid for; INVALID covers a missing, unreadable or
   unverifiable licence; NONE should be unreachable and is refused anyway. Nothing renders
   partially -- see ff-licence.h for why there is no expiry state to be lenient about. */
static bool licence_blocks(const struct ff_pack *pk)
{
	if (!pk || !pk->licensed)
		return false;
	return pk->licence.state == FF_LIC_NEWER || pk->licence.state == FF_LIC_INVALID ||
	       pk->licence.state == FF_LIC_NONE;
}

/* --------------------------------------------------------------- list filling */
/* Every populate_* below reads in->packs and so runs with state_lock held, except
   populate_audio_sources, which reads none of the instance and takes libobs's sources_mutex. */

/* A source draws its own canvas (visualizer/overlay presets); a filter reworks another source's
   image (effects presets). Offering the wrong kind would load a stack that has nothing to read. */
static bool preset_kind_matches(const struct ff_preset *pr, enum ff_kind kind)
{
	switch (kind) {
	case FF_KIND_FILTER:
		return !strcmp(pr->kind, "effects");
	case FF_KIND_TRANSITION:
		return !strcmp(pr->kind, "transition");
	case FF_KIND_SOURCE:
	default:
		return !strcmp(pr->kind, "visualizer") || !strcmp(pr->kind, "overlay");
	}
}

static bool pack_has_a_preset_for(const struct ff_pack *pk, enum ff_kind kind)
{
	for (size_t i = 0; i < pk->npresets; i++)
		if (preset_kind_matches(&pk->presets[i], kind))
			return true;
	return false;
}

/* Only packs this source could actually use. The preset list has always filtered by kind; the
   pack list did not, so every pack was offered to everything and picking the wrong one led to an
   empty preset dropdown and no explanation. The alerts pack is the plain case -- it declares
   kinds ["alert"], belongs to the alerts source, and had no business in the visualizer's list at
   all. A filter had it worse: `basics` offered fifteen presets, none of them effects.

   `current` is kept whatever its kind, so opening the panel can never silently move a scene off
   the pack it was saved with -- a dropdown that does not contain the stored value is one OBS may
   snap to its first entry. */
static void populate_pack_list(struct ff_instance *in, obs_property_t *p, const char *current)
{
	if (!p)
		return;
	obs_property_list_clear(p);
	for (size_t i = 0; i < in->packs.n; i++) {
		const struct ff_pack *pk = &in->packs.packs[i];
		if (pack_has_a_preset_for(pk, in->kind) || (current && !strcmp(pk->id, current)))
			obs_property_list_add_string(p, pk->name, pk->id);
	}
}

/* Fills the list and reports whether `want` is one of the entries, plus the first entry's id.
   Both answers come from the same walk because they must agree: asking a second time, later, is
   how the list and the selection drift apart. */
static void populate_preset_list(struct ff_instance *in, obs_property_t *p, const char *pack_id, const char *want,
				 bool *want_present, const char **first_id)
{
	if (want_present)
		*want_present = false;
	if (first_id)
		*first_id = NULL;
	if (!p)
		return;
	obs_property_list_clear(p);
	const struct ff_pack *pk = ff_packs_find(&in->packs, pack_id ? pack_id : "");
	if (!pk)
		return;
	for (size_t i = 0; i < pk->npresets; i++) {
		if (!preset_kind_matches(&pk->presets[i], in->kind))
			continue;
		obs_property_list_add_string(p, pk->presets[i].name, pk->presets[i].id);
		if (first_id && !*first_id)
			*first_id = pk->presets[i].id;
		if (want_present && want && !strcmp(want, pk->presets[i].id))
			*want_present = true;
	}
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
	if (!p)
		return;
	obs_property_list_clear(p);
	obs_enum_sources(add_audio_source, p);
}

/* ------------------------------------------------------------------ callbacks */
/* All of these run on the UI/RPC thread. */

static bool on_pack_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(p);
	struct ff_instance *in = obs_properties_get_param(props);
	if (!in)
		return false;
	const char *want = obs_data_get_string(s, S_PRESET);
	bool have = false;
	const char *first = NULL;
	pthread_mutex_lock(&in->state_lock);
	populate_preset_list(in, obs_properties_get(props, S_PRESET), obs_data_get_string(s, S_PACK), want, &have,
			     &first);
	pthread_mutex_unlock(&in->state_lock);

	/* Repair the selection ONLY when it is genuinely not in this pack.
	   Rewriting unconditionally is what the previous version refused to do, for a good reason:
	   this callback also runs on every properties OPEN, and a setting rewritten there races the
	   video thread's update() and leaves the combo showing one preset while Status names
	   another. Guarding on "the preset is not in the list" keeps that: on a normal open the
	   preset IS in the list, nothing is written, and the race cannot happen.
	   What it fixes is the case that has no other way out. Switching the Pack combo leaves the
	   old pack's preset id selected, and the source then draws NOTHING -- measured: set pack to
	   'ember' with 'bars' selected and the log says "Preset 'bars' is not in pack 'ember'" while
	   the canvas stays empty. The only visible symptom is a source that stopped working, which
	   is indistinguishable from the plugin being broken.
	   No obs_properties_apply_settings() either: it runs every modified callback, this one
	   included, and would recurse without end. Returning true is what refreshes the page. */
	if (!have && first) {
		obs_data_set_string(s, S_PRESET, first);
		obs_log(LOG_INFO, "preset '%s' is not in pack '%s'; selected '%s'", want,
			obs_data_get_string(s, S_PACK), first);
	}
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

	/* spawns unzip and parses the result: far too slow to hold state_lock across */
	char msg[512] = {0};
	bool ok = ff_packs_install_zip(zip, msg, sizeof msg);
	obs_log(ok ? LOG_INFO : LOG_WARNING, "install '%s': %s: %s", zip, ok ? "ok" : "refused", msg);

	pthread_mutex_lock(&in->state_lock);
	/* its own field, not in->status: the source update that follows this callback runs on the
	   video thread and rewrites status, which would swallow a refusal before it was ever shown */
	snprintf(in->install_msg, sizeof in->install_msg, "%s", msg);
	in->install_failed = !ok;
	pthread_mutex_unlock(&in->state_lock);

	/* one shot: left set, the picker would reinstall on every later update() */
	obs_data_set_string(s, S_INSTALL, "");
	refresh_packs(in);

	pthread_mutex_lock(&in->state_lock);
	populate_pack_list(in, obs_properties_get(props, S_PACK), obs_data_get_string(s, S_PACK));
	populate_preset_list(in, obs_properties_get(props, S_PRESET), obs_data_get_string(s, S_PACK), NULL, NULL, NULL);
	pthread_mutex_unlock(&in->state_lock);
	return true;
}

static bool on_reload(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	struct ff_instance *in = data;
	if (!in)
		return false;
	refresh_packs(in);

	pthread_mutex_lock(&in->state_lock);
	/* the authoring loop: the ids have not changed but the files on disk have, so ask update()
	   for a reload without pretending the preset switched (that would wipe the user's knobs) */
	in->reload_pending = true;
	populate_pack_list(in, obs_properties_get(props, S_PACK), in->pack_id);
	populate_preset_list(in, obs_properties_get(props, S_PRESET), in->pack_id, NULL, NULL, NULL);
	pthread_mutex_unlock(&in->state_lock);

	/* libobs defers a video source's update to the video thread; calling ff_instance_update from
	   here would run it on two threads at once, so the flag is what carries the request */
	obs_source_update(in->self, NULL);
	return true;
}

/* -------------------------------------------------------------- status lines */

static void add_info(obs_properties_t *props, const char *key, const char *text, enum obs_text_info_type type)
{
	obs_property_t *p = obs_properties_add_text(props, key, text, OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, type);
}

/* Spec §3.9: every refusal reaches the user as one sentence here. The list is never empty, so a
   working instance is visibly working rather than indistinguishable from a silent one.
   `audio_msg` is computed by the caller before it takes state_lock, because ff_audio_status takes
   ff_audio's own conn_lock and that must never be nested under ours. Call with state_lock held. */
static void add_status_lines(struct ff_instance *in, obs_properties_t *props, const char *audio_msg)
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
	if (audio_msg && audio_msg[0]) {
		add_info(props, "status.audio", audio_msg, OBS_TEXT_INFO_WARNING);
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

/* Call with state_lock held. */
static void add_licence_line(struct ff_instance *in, obs_properties_t *props)
{
	const struct ff_pack *pk = ff_packs_find(&in->packs, in->pack_id);
	if (!pk)
		return; /* no pack to describe; the Status line already says which one is missing */
	char line[320];
	licence_sentence(pk, line, sizeof line);
	/* Every state licence_blocks() names renders nothing, so there is no middle tier to show:
	   a warning colour next to a black frame would read as "still working". */
	enum obs_text_info_type type = licence_blocks(pk) ? OBS_TEXT_INFO_ERROR : OBS_TEXT_INFO_NORMAL;
	add_info(props, "licence", line, type);
}

/* Call with state_lock held. */
static void add_heavy_line(struct ff_instance *in, obs_properties_t *props)
{
	const struct ff_pack *pk = ff_packs_find(&in->packs, in->pack_id);
	const struct ff_preset *pr = ff_pack_find_preset(pk, in->preset_id);
	if (pr && pr->heavy)
		add_info(props, "heavy", obs_module_text("Foxfire.Preset.Heavy"), OBS_TEXT_INFO_WARNING);
}

/* ------------------------------------------------------------------ lifecycle */

/* The two per-source procedures. See ff-dock-proto.h for why they exist and why they are not a Qt
   feature: proc_handler is bare libobs, so this tap is in every build and anything holding an
   obs_source_t * can read it -- a dock, an obs-websocket script, another plugin.
   Both run on WHATEVER THREAD CALLS THEM, which in practice is the caller's UI or timer thread.
   That is a third thread reaching an ff_instance, so each takes exactly the lock the threading
   contract above assigns to what it touches, and neither touches anything the contract leaves
   unlocked (in->frame, in->width/height, in->dt are single-writer by construction and stay that
   way -- the frame here comes from the audio tap's own seqlock, not from in->frame). */
static void ff_proc_meter_read(void *data, calldata_t *cd)
{
	struct ff_instance *in = data;
	struct ff_frame *out = calldata_ptr(cd, FF_CD_OUT_FRAME);
	/* No lock: ff_audio_read goes through the handoff seqlock, which is safe to read from any
	   number of threads at once and never blocks the audio thread that writes it. */
	calldata_set_bool(cd, FF_CD_VALID, out && ff_audio_read(in->audio, out));
	/* Published alongside the frame, not derived from it: a silent frame and a frame that never
	   arrived are byte-identical, so the only evidence a reader has that audio is still FLOWING
	   is that this number moved since last time. */
	calldata_set_int(cd, FF_CD_SEQ, (long long)ff_audio_seq(in->audio));
}

static void ff_proc_dock_status(void *data, calldata_t *cd)
{
	struct ff_instance *in = data;
	struct ff_dock_status *out = calldata_ptr(cd, FF_CD_OUT_STATUS);
	if (!out)
		return;
	memset(out, 0, sizeof *out);
	pthread_mutex_lock(&in->state_lock);
	snprintf(out->pack_id, sizeof out->pack_id, "%s", in->pack_id);
	snprintf(out->preset_id, sizeof out->preset_id, "%s", in->preset_id);
	snprintf(out->status, sizeof out->status, "%s", in->status);
	snprintf(out->install_msg, sizeof out->install_msg, "%s", in->install_msg);
	out->install_failed = in->install_failed;
	pthread_mutex_unlock(&in->state_lock);
	/* AFTER the unlock, never inside it: ff_audio_describe takes ff_audio's conn_lock, and the
	   lock order this file documents is that conn_lock is taken OUTSIDE state_lock, never nested
	   under it. Nesting here would invert it against every other path and deadlock against a
	   configure() running the other way round. */
	ff_audio_describe(in->audio, &out->audio_mode, out->audio_source, sizeof out->audio_source, out->audio_msg,
			  sizeof out->audio_msg);
	out->audio_ok = out->audio_msg[0] == 0;
}

struct ff_instance *ff_instance_create(obs_data_t *settings, obs_source_t *self, enum ff_kind kind)
{
	struct ff_instance *in = bzalloc(sizeof *in);
	pthread_mutex_init(&in->state_lock, NULL);
	in->self = self;
	in->kind = kind;
	in->audio = ff_audio_create();
	/* Registered for the source's lifetime and never removed: callback/proc.h has no per-proc
	   unregister, only proc_handler_destroy for the whole handler, which the obs_source_t owns
	   and frees with itself. That is safe by construction rather than by luck -- reaching this
	   handler from outside requires a live strong reference to the source, and libobs cannot be
	   freeing the source (and with it the `in` captured here) while one is held. It is the same
	   refcount discipline ff-audio.c's connect_source/have_live_source already depends on. */
	proc_handler_t *ph = obs_source_get_proc_handler(self);
	proc_handler_add(ph, "void " FF_PROC_METER_READ "(in ptr out_frame, out bool valid, out int seq)", ff_proc_meter_read, in);
	proc_handler_add(ph, "void " FF_PROC_DOCK_STATUS "(in ptr out_status)", ff_proc_dock_status, in);
	refresh_packs(in);
	obs_enter_graphics();
	in->renderer = ff_renderer_create();
	if (ff_needs_capture(kind)) {
		in->capture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		/* the one GPU object created outside ff_renderer_create; flt_render (ff-filter.c) resets
		   and begins it every frame with no null check of its own, so a create failure here must
		   be reported now, at create -- ff_renderer_render's ping/pong guard is the pattern this
		   follows (see ff-layers.c), and flt_render's own guard is what actually keeps a failed
		   create from being a null dereference on the video thread. */
		ff_require(in->capture, "the filter's capture render target");
		if (!in->capture) {
			snprintf(in->status, sizeof in->status, "%s",
				 obs_module_text("Foxfire.Status.GraphicsUnavailable"));
		}
	}
	obs_leave_graphics();
	ff_instance_update(in, settings);
	return in;
}

void ff_instance_destroy(struct ff_instance *in)
{
	if (!in)
		return;
	/* libobs has stopped ticking and rendering this source before it calls destroy, so no other
	   thread is inside the instance here; the lock is torn down last all the same */
	ff_audio_destroy(in->audio);
	obs_enter_graphics();
	ff_renderer_destroy(in->renderer);
	if (in->capture)
		gs_texrender_destroy(in->capture);
	obs_leave_graphics();
	ff_packs_free(&in->packs);
	pthread_mutex_destroy(&in->state_lock);
	bfree(in);
}

void ff_instance_update(struct ff_instance *in, obs_data_t *s)
{
	if (!in || !s)
		return;
	const char *pack = obs_data_get_string(s, S_PACK);
	const char *preset = obs_data_get_string(s, S_PRESET);
	/* a filter has no S_WIDTH/S_HEIGHT settings -- its size comes from its target every frame
	   (flt_render sets in->width/height before calling ff_instance_render), so reading them here
	   would either read stale zeros or, worse, some other key's leftover value */
	if (ff_owns_canvas(in->kind)) {
		in->width = (uint32_t)obs_data_get_int(s, S_WIDTH);
		in->height = (uint32_t)obs_data_get_int(s, S_HEIGHT);
	}

	struct ff_analysis_params ap = {
		.release_ms = (float)obs_data_get_double(s, S_RELEASE),
		.beat_sensitivity = (float)obs_data_get_double(s, S_BEAT),
		.gain_db = (float)obs_data_get_double(s, S_GAIN),
	};
	/* takes ff_audio's conn_lock: kept outside state_lock so the two never nest */
	ff_audio_configure(in->audio, (enum ff_audio_mode)obs_data_get_int(s, S_AUDIO_MODE),
			   obs_data_get_string(s, S_AUDIO_SOURCE), &ap);

	/* pk and pr point into in->packs, which the UI thread may replace at any moment, so the lock
	   is held from the lookup all the way through the load that consumes them */
	pthread_mutex_lock(&in->state_lock);
	in->status[0] = 0;
	const struct ff_pack *pk = ff_packs_find(&in->packs, pack);
	const struct ff_preset *pr = ff_pack_find_preset(pk, preset);
	if (!pk) {
		fmt_text(in->status, sizeof in->status, "Foxfire.Pack.Missing", pack, NULL);
	} else if (licence_blocks(pk)) {
		licence_sentence(pk, in->status, sizeof in->status);
		pr = NULL; /* the gate: a blocked pack loads zero layers */
	} else if (!pr) {
		fmt_text(in->status, sizeof in->status, "Foxfire.Preset.Missing", preset, pack);
	}
	char warn[sizeof in->status];
	snprintf(warn, sizeof warn, "%s", in->status); /* logged after the lock is dropped */

	/* On the first update there is no previous preset, so nothing is stale: treating a scene
	   load as a preset switch would erase the layer values the user saved with the scene. */
	bool changed = in->initialised && (strcmp(in->pack_id, pack) != 0 || strcmp(in->preset_id, preset) != 0);
	snprintf(in->pack_id, sizeof in->pack_id, "%s", pack);
	snprintf(in->preset_id, sizeof in->preset_id, "%s", preset);
	bool reload = changed || in->reload_pending || !in->initialised;
	in->reload_pending = false;
	if (changed)
		erase_layer_keys(s);

	obs_enter_graphics();
	if (reload) {
		ff_renderer_load(in->renderer, pk, pr); /* a NULL preset leaves zero layers */
		/* only in the load branch: run every update it would record the user's own values as
		   the obs_data defaults, and Restore Defaults could never get back to the preset */
		ff_renderer_set_defaults(in->renderer, s);
	}
	ff_renderer_apply_settings(in->renderer, s);
	obs_leave_graphics();
	in->initialised = true;
	pthread_mutex_unlock(&in->state_lock);

	if (warn[0])
		obs_log(LOG_WARNING, "pack '%s' preset '%s': %s", pack, preset, warn);
}

void ff_instance_defaults(obs_data_t *s, enum ff_kind kind)
{
	obs_data_set_default_string(s, S_PACK, "demo");
	/* each kind's default preset has to be one IT can load: the pack dropdown and the preset
	   list both filter by kind, so a default of the wrong kind would leave a new source
	   pointing at a preset that is not in its own list. */
	static const char *DEFAULT_PRESET[] = {
		[FF_KIND_SOURCE] = "bars",
		[FF_KIND_FILTER] = "glow-only",
		[FF_KIND_TRANSITION] = "dissolve",
	};
	obs_data_set_default_string(s, S_PRESET, DEFAULT_PRESET[kind]);
	/* only a source has S_WIDTH/S_HEIGHT properties (ff_instance_properties skips them
	   otherwise) and ff_instance_update ignores the keys for the other kinds too -- setting
	   defaults nobody ever reads would just be a landmine for a future reader wondering why a
	   filter's saved settings carry a width/height it never respects */
	if (ff_owns_canvas(kind)) {
		obs_data_set_default_int(s, S_WIDTH, 1920);
		obs_data_set_default_int(s, S_HEIGHT, 1080);
	}
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

	/* takes ff_audio's conn_lock, so it happens before state_lock, not under it */
	char audio_msg[256];
	if (ff_audio_status(in->audio, audio_msg, sizeof audio_msg))
		audio_msg[0] = 0;

	pthread_mutex_lock(&in->state_lock);

	obs_property_t *packs = obs_properties_add_list(props, S_PACK, obs_module_text("Foxfire.Pack"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	populate_pack_list(in, packs, in->pack_id);
	obs_property_set_modified_callback(packs, on_pack_changed);

	obs_property_t *presets = obs_properties_add_list(props, S_PRESET, obs_module_text("Foxfire.Preset"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	populate_preset_list(in, presets, in->pack_id, NULL, NULL, NULL);
	obs_property_set_modified_callback(presets, on_preset_changed);

	if (ff_owns_canvas(in->kind)) {
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

	obs_properties_add_float_slider(props, S_GAIN, obs_module_text("Foxfire.Gain"), -24.0, 24.0, 0.1);
	obs_properties_add_float_slider(props, S_RELEASE, obs_module_text("Foxfire.Release"), 20.0, 2000.0, 5.0);
	obs_properties_add_float_slider(props, S_BEAT, obs_module_text("Foxfire.BeatSensitivity"), 0.1, 3.0, 0.05);

	obs_property_t *install = obs_properties_add_path(props, S_INSTALL, obs_module_text("Foxfire.InstallPack"),
							  OBS_PATH_FILE, "Pack (*.zip)", NULL);
	obs_property_set_modified_callback(install, on_install_changed);
	obs_properties_add_button2(props, "reload", obs_module_text("Foxfire.Reload"), on_reload, in);

	add_status_lines(in, props, audio_msg);
	add_licence_line(in, props);
	add_heavy_line(in, props);
	ff_renderer_add_properties(in->renderer, props); /* layer errors, then the preset's own knobs */

	pthread_mutex_unlock(&in->state_lock);

	/* enumerating sources takes libobs's sources_mutex and reads none of the instance: it goes
	   after the unlock so state_lock is never the outer of that pair */
	populate_audio_sources(asrc);
	return props;
}

gs_texture_t *ff_instance_render(struct ff_instance *in, gs_texture_t *input, uint32_t w, uint32_t h)
{
	/* 0 progress and no pair: a visualizer and a filter are not "doing" something with a
	   beginning and an end -- that is the alert source's and the transition's business. */
	return ff_instance_render_ex(in, input, NULL, 0.f, w, h);
}

gs_texture_t *ff_instance_render_ex(struct ff_instance *in, gs_texture_t *input, const struct ff_pair_tex *pair,
				    float progress, uint32_t w, uint32_t h)
{
	if (!in)
		return NULL;
	/* a failed read can have torn the destination, so it lands in a local: the instance keeps
	   the last frame that was read whole */
	struct ff_frame f;
	if (ff_audio_read(in->audio, &f))
		in->frame = f;
	return ff_renderer_render(in->renderer, &in->frame, progress, input, pair, w, h, in->dt);
}
