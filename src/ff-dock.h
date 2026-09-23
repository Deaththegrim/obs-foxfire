/*
Foxfire
Copyright (C) 2026 KitsuneStudio

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

/* The dock, reached from plugin-main.c.
 *
 * No Qt and no obs-frontend-api types cross this header, deliberately: plugin-main.c is plain C
 * and compiled as C, and the whole dock is behind FF_HAVE_DOCK. Two functions is the entire
 * surface -- everything else lives in ff-dock.cpp, which is the only translation unit in the
 * project that knows Qt exists.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Asks OBS to build the dock once its frontend is ready. Safe to call from obs_module_load, which
   is far too early to create a widget -- this only registers the callback that will. */
void ff_dock_register(void);
void ff_dock_unregister(void);

#ifdef __cplusplus
}
#endif
