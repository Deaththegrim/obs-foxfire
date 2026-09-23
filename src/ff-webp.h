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

/* Animated WebP, decoded by us.
 *
 * libobs cannot do this and never will through gs_image_file: it decodes GIF separately with
 * libnsgif and routes every OTHER image through ffmpeg, and ffmpeg's webp decoder does not
 * implement the animated VP8X/ANMF container at all -- `ffprobe` refuses such a file on its own,
 * before OBS is involved. So an animated WebP did not merely fail to animate, it failed to LOAD,
 * and the source rendered blank.
 *
 * GIF is not an answer for art: 256 colours and one bit of alpha, which destroys exactly the
 * gradients and soft edges a stream overlay is made of. So the engine carries libwebp's
 * WebPAnimDecoder and plays the real thing.
 *
 * MEMORY. The compressed bytes are kept and the frames are NOT. A 1920x1080 sixty-frame animation
 * is about 500 MB as raw RGBA and a few hundred KB as WebP; WebPAnimDecoder reconstructs one full
 * canvas at a time, so that is all that is ever held. It also means playback is sequential --
 * seeking backwards is a Reset and replay, which is what looping does anyway.
 */

#include <stdbool.h>
#include <stdint.h>

struct ff_webp;

/* Opens `path` as an ANIMATED WebP.
 *
 * Returns NULL for anything else -- a still WebP, a PNG, a missing or malformed file, or a WebP
 * whose animation is a single frame. That is not an error and must not be logged as one: it is the
 * caller's signal to fall back to libobs's loader, which handles every one of those correctly.
 * Only a file that genuinely has more than one frame is taken over. */
struct ff_webp *ff_webp_open(const char *path);

uint32_t ff_webp_width(const struct ff_webp *a);
uint32_t ff_webp_height(const struct ff_webp *a);
uint32_t ff_webp_frames(const struct ff_webp *a);

/* The frame that should be on screen after `dt_ns` more time.
 *
 * Returns the RGBA canvas (width*height*4) ONLY when the frame changed, and NULL when it did not,
 * so the caller uploads a texture only on a real frame change rather than every tick. Pass dt_ns
 * = 0 to get the first frame without advancing, which is what the initial upload wants. */
const uint8_t *ff_webp_advance(struct ff_webp *a, uint64_t dt_ns);

void ff_webp_close(struct ff_webp *a);
