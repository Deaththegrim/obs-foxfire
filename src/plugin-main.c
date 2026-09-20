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
#include <time.h>
#include "ff-pack.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info ff_source_info;

const char *obs_module_name(void)
{
	return "Foxfire";
}

const char *obs_module_description(void)
{
	return "Audio-reactive layered shader visualizer and effects";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
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
		ff_packs_scan(&l, (int64_t)time(NULL));
		ff_packs_free(&l);
	}
	obs_register_source(&ff_source_info);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
