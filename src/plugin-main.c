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

#include <obs-module.h>
#include <plugin-support.h>
#include "ff-pack.h"
#include "ff-dock-proto.h"
#if defined(FF_HAVE_DOCK)
#include "ff-dock.h"
#endif
#include "ff-frame.h"
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info ff_source_info;
extern struct obs_source_info ff_filter_info;
extern struct obs_source_info ff_transition_info;

const char *obs_module_name(void)
{
	return "Foxfire";
}

const char *obs_module_description(void)
{
	return "Audio-reactive layered shader visualizer and effects";
}

/* The per-source meter tap (ff-props.c's proc handlers), exercised against a REAL obs_source_t
   inside a REAL OBS process. The unit tests cannot reach this: ff_instance_create calls
   obs_enter_graphics, so there is no instance to call a proc handler on outside a running OBS,
   and obs-websocket has no generic "call this proc" request to drive it from the outside.
   So the plugin proves it about itself, the same way FOXFIRE_INSTALL_ZIP below does, and
   tools/dock-proof.py boots OBS under Xvfb and reads the verdict out of the log.

   On a tick rather than in obs_module_load: creating a source needs OBS further up than module
   load gets, and the first tick is the earliest point everything it touches exists. It runs once
   and unregisters itself. */
static void proc_proof_tick(void *param, float seconds)
{
	(void)param;
	(void)seconds;
	obs_remove_tick_callback(proc_proof_tick, NULL);

	int fails = 0, checks = 0;
#define PP_CHECK(name, cond)                                                     \
	do {                                                                     \
		checks++;                                                        \
		bool ok_ = (cond);                                               \
		if (!ok_)                                                        \
			fails++;                                                 \
		obs_log(LOG_INFO, "proc-proof: [%s] %s", ok_ ? "PASS" : "FAIL", name); \
	} while (0)

	obs_data_t *st = obs_data_create();
	obs_source_t *src = obs_source_create("foxfire_visualizer", "ff_proc_proof_src", st, NULL);
	obs_data_release(st);
	PP_CHECK("a foxfire_visualizer can be created", src != NULL);
	if (!src) {
		obs_log(LOG_INFO, "proc-proof: %d/%d passed", checks - fails, checks);
		return;
	}

	proc_handler_t *ph = obs_source_get_proc_handler(src);
	PP_CHECK("the source has a proc handler", ph != NULL);

	/* the meter tap */
	struct ff_frame f;
	memset(&f, 0xAB, sizeof f); /* poison: a handler that writes nothing must be detectable */
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, FF_CD_OUT_FRAME, &f);
	bool called = proc_handler_call(ph, FF_PROC_METER_READ, &cd);
	PP_CHECK("ff_meter_read is registered and dispatches", called);
	/* `valid` false is CORRECT here -- a source nobody has fed has published no frame yet. What
	   is being proved is that the call reaches the handler and answers, not that audio flowed. */
	PP_CHECK("and it answers with a valid flag", calldata_bool(&cd, FF_CD_VALID) == false);
	calldata_free(&cd);

	/* a missing out-pointer must be refused, not dereferenced */
	calldata_init(&cd);
	calldata_set_ptr(&cd, FF_CD_OUT_FRAME, NULL);
	PP_CHECK("ff_meter_read survives a null out pointer", proc_handler_call(ph, FF_PROC_METER_READ, &cd));
	PP_CHECK("and reports it as not valid", calldata_bool(&cd, FF_CD_VALID) == false);
	calldata_free(&cd);

	/* the status tap */
	struct ff_dock_status ds;
	memset(&ds, 0xAB, sizeof ds);
	calldata_init(&cd);
	calldata_set_ptr(&cd, FF_CD_OUT_STATUS, &ds);
	PP_CHECK("ff_dock_status is registered and dispatches", proc_handler_call(ph, FF_PROC_DOCK_STATUS, &cd));
	calldata_free(&cd);
	/* the poison is gone, so the handler really wrote the struct rather than the call merely
	   returning true */
	PP_CHECK("and it filled the status struct", ds.audio_mode == FF_AUDIO_MASTER);
	PP_CHECK("a master-mode source names no audio source", ds.audio_source[0] == 0);
	PP_CHECK("and reports its audio as healthy", ds.audio_ok && ds.audio_msg[0] == 0);

	PP_CHECK("an unknown proc is refused", !proc_handler_call(ph, "ff_no_such_proc", NULL));

	/* The fault path, which the healthy source above cannot show: an instance told to follow an
	   audio source that does not exist. Built with those settings at create rather than updated
	   afterwards, because obs_source_update is deferred to a later tick and this runs inside one.
	   This is the half a panel needs most -- "which audio source is feeding it" is only useful
	   when it can say "none of them, and here is why". */
	obs_data_t *bad = obs_data_create();
	obs_data_set_int(bad, "audio_mode", 1); /* FF_AUDIO_SOURCE */
	obs_data_set_string(bad, "audio_source", "ff-no-such-audio-source");
	obs_source_t *orphan = obs_source_create("foxfire_visualizer", "ff_proc_proof_orphan", bad, NULL);
	obs_data_release(bad);
	PP_CHECK("a source following a missing audio source can be created", orphan != NULL);
	if (orphan) {
		struct ff_dock_status os;
		memset(&os, 0xAB, sizeof os);
		calldata_init(&cd);
		calldata_set_ptr(&cd, FF_CD_OUT_STATUS, &os);
		proc_handler_call(obs_source_get_proc_handler(orphan), FF_PROC_DOCK_STATUS, &cd);
		calldata_free(&cd);
		PP_CHECK("it reports the mode it was given", os.audio_mode == FF_AUDIO_SOURCE);
		PP_CHECK("and names the source it was told to follow",
			 strcmp(os.audio_source, "ff-no-such-audio-source") == 0);
		PP_CHECK("and reports the audio as unhealthy", !os.audio_ok);
		PP_CHECK("with a sentence naming it", strstr(os.audio_msg, "ff-no-such-audio-source") != NULL);
		obs_source_release(orphan);
	}

	obs_source_release(src);
	obs_log(LOG_INFO, "proc-proof: %d/%d passed", checks - fails, checks);
#undef PP_CHECK
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	if (getenv("FOXFIRE_PROC_PROOF"))
		obs_add_tick_callback(proc_proof_tick, NULL);
	/* diagnostic hook: set FOXFIRE_INSTALL_ZIP=<path to a pack .zip> to exercise
	   ff_packs_install_zip() at startup, before the scan below runs */
	const char *install_zip = getenv("FOXFIRE_INSTALL_ZIP");
	if (install_zip) {
		char msg[512] = {0};
		bool ok = ff_packs_install_zip(install_zip, msg, sizeof msg);
		obs_log(LOG_INFO, "install: %s: %s", ok ? "ok" : "refused", msg);
	}
	/* startup scan: logs pack counts; the source keeps its own list (Task 8) */
	{
		struct ff_pack_list l;
		ff_packs_scan(&l);
		ff_packs_free(&l);
	}
	obs_register_source(&ff_source_info);
	obs_register_source(&ff_filter_info);
	obs_register_source(&ff_transition_info);
#if defined(FF_HAVE_DOCK)
	/* Only registers the frontend callback; the widget cannot exist this early. */
	ff_dock_register();
#endif
	return true;
}

void obs_module_unload(void)
{
#if defined(FF_HAVE_DOCK)
	ff_dock_unregister();
#endif
	obs_log(LOG_INFO, "plugin unloaded");
}
