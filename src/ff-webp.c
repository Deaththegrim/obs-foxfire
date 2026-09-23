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

#include "ff-webp.h"

#include <obs-module.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <webp/decode.h>
#include <webp/demux.h>

#include <string.h>

struct ff_webp {
	/* The COMPRESSED file, held for the decoder's whole life. demux.h is explicit that the
	   WebPData "must outlive the lifetime of the output WebPAnimDecoder object" -- freeing it
	   after WebPAnimDecoderNew leaves the decoder pointing at released memory. */
	uint8_t *bytes;
	size_t nbytes;

	WebPAnimDecoder *dec;
	WebPAnimInfo info;

	uint64_t elapsed_ns;  /* since the current loop began */
	int frame_end_ms;     /* when the frame now showing stops being current */
	bool have_frame;      /* a frame has been fetched at least once */
	bool exhausted;       /* the decoder ran out; the next advance restarts the loop */
};

static bool read_whole_file(const char *path, uint8_t **out, size_t *out_n)
{
	*out = NULL;
	*out_n = 0;
	/* os_get_file_size / os_fopen rather than stat + fopen: os_stat is a macro for plain stat on
	   some platforms and os_fopen is what handles a UTF-8 path on Windows, where fopen does not. */
	const int64_t size = os_get_file_size(path);
	if (size <= 0)
		return false;
	FILE *f = os_fopen(path, "rb");
	if (!f)
		return false;
	uint8_t *buf = bmalloc((size_t)size);
	const size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		bfree(buf);
		return false;
	}
	*out = buf;
	*out_n = got;
	return true;
}

struct ff_webp *ff_webp_open(const char *path)
{
	if (!path || !*path)
		return NULL;

	uint8_t *bytes = NULL;
	size_t nbytes = 0;
	if (!read_whole_file(path, &bytes, &nbytes))
		return NULL;

	/* Cheapest possible rejection first: anything that is not a WebP at all, and any WebP that
	   carries no animation, is handed straight back so libobs's loader keeps it. This runs for
	   every texture a pack loads, so it must not be the expensive path for a PNG. */
	WebPBitstreamFeatures feat;
	if (WebPGetFeatures(bytes, nbytes, &feat) != VP8_STATUS_OK || !feat.has_animation) {
		bfree(bytes);
		return NULL;
	}

	WebPData data = {.bytes = bytes, .size = nbytes};
	WebPAnimDecoderOptions opts;
	if (!WebPAnimDecoderOptionsInit(&opts)) {
		bfree(bytes);
		return NULL;
	}
	/* RGBA, because that is what gs_texture_create takes and what every Foxfire shader samples.
	   WebPAnimDecoderGetNext then hands back a fully reconstructed canvas -- frame disposal and
	   blending already applied -- rather than a sub-rectangle we would have to composite. */
	opts.color_mode = MODE_RGBA;
	opts.use_threads = 0;

	WebPAnimDecoder *dec = WebPAnimDecoderNew(&data, &opts);
	if (!dec) {
		bfree(bytes);
		return NULL;
	}

	struct ff_webp *a = bzalloc(sizeof *a);
	a->bytes = bytes;
	a->nbytes = nbytes;
	a->dec = dec;
	if (!WebPAnimDecoderGetInfo(dec, &a->info) || a->info.frame_count < 2 ||
	    !a->info.canvas_width || !a->info.canvas_height) {
		/* A single-frame "animation" is a still image with extra steps; libobs renders those
		   correctly and cheaply, so it keeps them. */
		ff_webp_close(a);
		return NULL;
	}
	obs_log(LOG_INFO, "webp: '%s' is animated: %ux%u, %u frames", path, a->info.canvas_width,
		a->info.canvas_height, a->info.frame_count);
	return a;
}

uint32_t ff_webp_width(const struct ff_webp *a)
{
	return a ? a->info.canvas_width : 0;
}

uint32_t ff_webp_height(const struct ff_webp *a)
{
	return a ? a->info.canvas_height : 0;
}

uint32_t ff_webp_frames(const struct ff_webp *a)
{
	return a ? a->info.frame_count : 0;
}

/* Pulls the next frame and records when it stops being current. `timestamp` from
   WebPAnimDecoderGetNext is the END of the frame in milliseconds, not its start -- so it is
   exactly the deadline to compare the elapsed clock against, with no per-frame duration
   arithmetic. */
static const uint8_t *pull(struct ff_webp *a)
{
	uint8_t *buf = NULL;
	int ts = 0;
	if (!WebPAnimDecoderGetNext(a->dec, &buf, &ts)) {
		a->exhausted = true;
		return NULL;
	}
	a->frame_end_ms = ts;
	a->have_frame = true;
	return buf;
}

const uint8_t *ff_webp_advance(struct ff_webp *a, uint64_t dt_ns)
{
	if (!a || !a->dec)
		return NULL;

	if (!a->have_frame) {
		/* the first upload; do not advance the clock for it */
		return pull(a);
	}

	a->elapsed_ns += dt_ns;
	const int64_t now_ms = (int64_t)(a->elapsed_ns / 1000000ULL);
	if (now_ms < (int64_t)a->frame_end_ms)
		return NULL; /* the frame on screen is still the right one */

	if (!WebPAnimDecoderHasMoreFrames(a->dec) || a->exhausted) {
		/* Loop. The clock rewinds by the length of the animation rather than to zero, so a
		   long frame that overran its deadline does not lose the overrun and drift the loop
		   slower every time round. */
		const uint64_t total_ns = (uint64_t)a->frame_end_ms * 1000000ULL;
		a->elapsed_ns = total_ns ? (a->elapsed_ns % total_ns) : 0;
		WebPAnimDecoderReset(a->dec);
		a->exhausted = false;
		a->frame_end_ms = 0;
		return pull(a);
	}
	return pull(a);
}

void ff_webp_close(struct ff_webp *a)
{
	if (!a)
		return;
	if (a->dec)
		WebPAnimDecoderDelete(a->dec);
	/* after the decoder, never before: it holds a pointer into these bytes */
	bfree(a->bytes);
	bfree(a);
}
