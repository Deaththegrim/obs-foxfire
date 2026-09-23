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
#include <stddef.h> /* offsetof, for the layout assertions at the bottom */

/* void ff_meter_read(in ptr out_frame, out bool valid)
   Copies this instance's most recent analysis frame into the caller's `struct ff_frame`.
   `valid` is false when nothing has been published yet or the reader lost the seqlock race, in
   which case out_frame is untouched and a meter should keep drawing what it already had. */
#define FF_PROC_METER_READ "ff_meter_read"
#define FF_CD_OUT_FRAME "out_frame"
#define FF_CD_VALID "valid"
/* The instance's publish counter, so a reader can tell a source being fed SILENCE from one nothing
   is feeding at all. Both look identical in the frame -- every band zero -- and the panel used to
   render them identically too: a flat meter under a grey "Master audio". */
#define FF_CD_SEQ "seq"

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

/* This struct is the ONLY thing that crosses the C/C++ boundary in this project: ff-props.c fills
   it, ff-dock.cpp reads it, and the calldata carries nothing but a void pointer to it -- so a
   layout disagreement between the two translation units would not be a compile error, it would be
   a dock quietly reading the wrong bytes. It is plausible that the two agree and plausible is not
   checkable, so it is checked. An enum in a struct is where they would actually diverge
   (-fshort-enums on one side and not the other), which is why the offset past it is pinned too. */
#ifdef __cplusplus
#define FF_STATIC_ASSERT static_assert
#else
#define FF_STATIC_ASSERT _Static_assert
#endif
FF_STATIC_ASSERT(sizeof(enum ff_audio_mode) == sizeof(int), "ff_audio_mode is not int-sized");
/* Measured, not assumed: 640 bytes of char arrays, install_failed, three bytes of padding, then
   the enum. Every field AFTER the enum is pinned because that is what a size change would move. */
FF_STATIC_ASSERT(offsetof(struct ff_dock_status, audio_mode) == 644, "ff_dock_status layout");
FF_STATIC_ASSERT(offsetof(struct ff_dock_status, audio_msg) == 904, "ff_dock_status layout");
FF_STATIC_ASSERT(offsetof(struct ff_dock_status, audio_ok) == 1160, "ff_dock_status layout");
FF_STATIC_ASSERT(sizeof(struct ff_dock_status) == 1164, "ff_dock_status layout");
