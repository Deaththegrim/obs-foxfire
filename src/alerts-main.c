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

/* A SEPARATE plugin from the visualizer, deliberately.
 *
 * Nobody should have to install all of Foxfire to use one part of it. This is its own .so, its
 * own entry in OBS's plugin list, its own enable/disable, and it can be installed with the
 * visualizer absent entirely. What it shares is the codebase: pack loading, licence verification
 * and the layer renderer all come from ff-core, and packs land in one shared directory
 * (ff_shared_config_path) so installing a pack through either plugin makes it visible to both.
 *
 * It logs under the same "[obs-foxfire]" tag as the rest of the family -- the log is read by
 * people and by tools/proof.py's scanner, and one tag for one project is right -- so every
 * message here names its own subsystem ("alerts: ...") instead.
 */

#include <obs-module.h>
#include <plugin-support.h>
#include "ff-alert.h"

extern struct obs_source_info ff_alert_source_info;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

const char *obs_module_name(void)
{
	return "Foxfire Alerts";
}

const char *obs_module_description(void)
{
	return "Native Twitch alerts: art, text and sound, with no browser source and no server";
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "alerts: plugin loaded successfully (version %s)", PLUGIN_VERSION);
	/* Resolved once, at load, so the answer is in the log BEFORE anyone adds a source and
	   wonders why their alert has no name on it. */
	ff_alert_text_kind();
	obs_register_source(&ff_alert_source_info);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "alerts: plugin unloaded");
}
