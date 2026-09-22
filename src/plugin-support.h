/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

void obs_log(int log_level, const char *format, ...);

/* blogva is NOT declared here, though the template shipped it that way. It belongs to libobs,
   which declares it in <util/base.h> as EXPORT -- and on Windows EXPORT is __declspec(dllimport)
   for a plugin consuming libobs. A second declaration with default linkage is not a duplicate
   MSVC forgives: `error C2375: 'blogva': redefinition; different linkage`, and the build stops.
   GCC accepts the same pair silently, which is why this only ever failed off Linux. The one
   caller is plugin-support.c, which includes <util/base.h> for LOG_WARNING anyway. */

#ifdef __cplusplus
}
#endif
