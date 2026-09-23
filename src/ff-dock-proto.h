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

/* What a Foxfire source will tell anyone holding an obs_source_t * about itself.
 *
 * Every instance owns its analysis privately and there is no registry of live instances, so
 * nothing outside an instance's own callbacks could see its meters. libobs already has the
 * channel for exactly this -- a per-source proc handler
 * (obs_source_get_proc_handler + proc_handler_add/call) -- so this is a use of the existing
 * mechanism rather than a new one, and it is the reason the plugin does not need to grow a global
 * list of instances just so a panel can draw a bar.
 *
 * NONE OF THIS IS QT. proc.h is bare libobs, so the tap compiles and is proved in the default
 * build with ENABLE_QT off. The dock is one consumer; obs-websocket scripts and any other plugin
 * are equally entitled to call these.
 *
 * The decl strings passed to proc_handler_add are documentation: proc_handler_call dispatches on
 * the NAME alone and nothing parses the "(in ptr ...)" grammar. So correctness here rests entirely
 * on both sides agreeing about the calldata keys, which is why the keys are #defined once, in this
 * header, included by the C side that registers them and the C++ side that calls them. Two string
 * literals typed at two call sites is the whole failure mode.
 *
 * Every call is CALLER-ALLOCATES: the caller passes a pointer to its own struct and the handler
 * fills it in. calldata has no blob-copy primitive (calldata.h offers set_ptr/set_bool and little
 * else), and a ~2.6 KB ff_frame has no business being marshalled field by field.
 */

#include "ff-audio.h" /* enum ff_audio_mode */
#include <stdbool.h>

/* void ff_meter_read(in ptr out_frame, out bool valid)
   Copies this instance's most recent analysis frame into the caller's `struct ff_frame`.
   `valid` is false when nothing has been published yet or the reader lost the seqlock race, in
   which case out_frame is untouched and a meter should keep drawing what it already had. */
#define FF_PROC_METER_READ "ff_meter_read"
#define FF_CD_OUT_FRAME "out_frame"
#define FF_CD_VALID "valid"

/* void ff_dock_status(in ptr out_status)
   Fills the caller's `struct ff_dock_status`. */
#define FF_PROC_DOCK_STATUS "ff_dock_status"
#define FF_CD_OUT_STATUS "out_status"

struct ff_dock_status {
	char pack_id[64];
	char preset_id[64];
	char status[256];      /* the current pack/preset's refusal sentence; empty when fine */
	char install_msg[256]; /* last "Install pack" result */
	bool install_failed;   /* a refusal must not read as a success */

	enum ff_audio_mode audio_mode;
	char audio_source[256]; /* meaningless in MASTER mode; empty there rather than stale */
	/* Verbatim from ff_audio_describe, which produces the same sentence the properties panel
	   shows. A panel and a dock that word the same fault differently are two bug reports. */
	char audio_msg[256];
	bool audio_ok;
};
