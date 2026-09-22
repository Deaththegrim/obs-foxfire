/* The alert source: text and sound, which the layer renderer has never had.
 *
 * An alert is not a visualizer. It draws a NAME -- hostile input, arriving from Twitch and
 * painted onto a live stream -- and it makes a NOISE. Neither is something a shader can do, so
 * this source is a composite: it owns two private child sources, one that rasterises text and one
 * that decodes audio, and it draws and mixes them itself.
 *
 * Why children rather than doing it ourselves, both measured rather than assumed (see
 * foxfire/research/alerts-rendering.md):
 *
 *   TEXT.  OBS's text sources have different kind names per platform -- text_ft2_source_v2 here,
 *   the text_gdiplus family on Windows -- but obs_enum_source_types() makes the set enumerable at
 *   RUNTIME, so nothing needs an #ifdef. We walk a candidate list and take the first kind that
 *   exists. That also survives OBS renaming a kind, which it has done twice (note the _v2/_v3
 *   suffixes), and when nothing matches it says so instead of drawing an empty rectangle.
 *
 *   SOUND.  ffmpeg_source already decodes whatever a streamer drops in. A private source sits in
 *   no scene, though, so nothing mixes its audio -- which is what OBS_SOURCE_COMPOSITE and the
 *   audio_render callback are for (obs-source.h:135). The alternative, obs_source_output_audio,
 *   is for a source that GENERATES PCM and would mean owning a decoder.
 *
 * A pack never names a child's settings keys. The text source's keys differ per platform (this
 * one has no `text` default at all, and the GDI+ one's key set differs again), so letting a pack
 * write them directly would make the same alert pack behave differently per OS. The engine owns
 * the vocabulary; ff_alert_text_apply maps it.
 */

#include "ff-alert.h"
#include <plugin-support.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <media-io/audio-io.h>
#include <string.h>
#include <stdint.h>

/* Tried in order. The Windows names come from the OBS source tree rather than from a machine we
   have run on, which is exactly why this is a list and not a constant: the code has to behave
   correctly when none of it matches. */
static const char *TEXT_KINDS[] = {"text_gdiplus_v3",    "text_gdiplus_v2", "text_gdiplus",
				   "text_ft2_source_v2", "text_ft2_source", NULL};

const char *ff_alert_text_kind(void)
{
	static const char *found = NULL;
	static bool looked = false;
	if (looked)
		return found;
	looked = true;
	for (size_t i = 0; TEXT_KINDS[i]; i++) {
		/* obs_get_source_output_flags returns 0 for a kind that is not registered; a real
		   text source always reports OBS_SOURCE_VIDEO, so 0 is an unambiguous "absent". */
		if (obs_get_source_output_flags(TEXT_KINDS[i]) != 0) {
			found = TEXT_KINDS[i];
			break;
		}
	}
	if (!found) {
		struct dstr tried = {0};
		for (size_t i = 0; TEXT_KINDS[i]; i++)
			dstr_catf(&tried, i ? ", %s" : "%s", TEXT_KINDS[i]);
		obs_log(LOG_WARNING,
			"alerts: this OBS registers none of the text sources we know how to drive "
			"(%s), so alerts will draw their art and sound but NO NAME. Report this with "
			"your OBS version -- it means the kind was renamed again.",
			tried.array ? tried.array : "");
		dstr_free(&tried);
	} else {
		obs_log(LOG_INFO, "alerts: drawing text with '%s'", found);
	}
	return found;
}

void ff_alert_text_apply(obs_source_t *text, const struct ff_alert_text *t)
{
	if (!text || !t)
		return;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", t->body);

	/* Both families read a "font" sub-object with the same field names, which is the one thing
	   they do agree on. Everything else is deliberately left at the source's own defaults
	   rather than guessed, so a key that means something different on the other platform
	   cannot quietly change the look. */
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", t->face[0] ? t->face : "Sans Serif");
	obs_data_set_int(font, "size", t->size > 0 ? t->size : 48);
	obs_data_set_int(font, "flags", t->bold ? OBS_FONT_BOLD : 0);
	obs_data_set_obj(s, "font", font);
	obs_data_release(font);

	/* colour: the ft2 source takes two (a vertical gradient); GDI+ takes color1 only. Setting
	   both to the same value is correct on both and needs no branch. */
	obs_data_set_int(s, "color", (long long)t->colour);
	obs_data_set_int(s, "color1", (long long)t->colour);
	obs_data_set_int(s, "color2", (long long)t->colour);
	obs_data_set_bool(s, "outline", t->outline);
	obs_data_set_bool(s, "drop_shadow", t->shadow);

	obs_source_update(text, s);
	obs_data_release(s);
}

size_t ff_alert_sanitise(const char *in, char *out, size_t cap)
{
	/* A username is hostile input drawn on a live stream. Three things matter, and none of
	   them is profanity (that is a policy list, and belongs above this):
	     - control characters, which the text sources handle inconsistently -- a newline turns a
	       one-line alert into a two-line one and pushes the rest of the layout off;
	     - bidirectional overrides and isolates, which visually reverse the text around a name:
	       "gift" + U+202E + "knarp" draws as "giftprank", a word the sender never typed;
	     - length, because "alerts with >400 characters break the system entirely" is a
	       documented failure of Twitch's own alerts (alerts-parity.md) and we inherit nothing.

	   Decided PER CHARACTER, never by copying and repairing afterwards. The first version cut
	   at the byte limit and then walked back off any trailing continuation bytes -- which also
	   stripped the last character off strings that were never truncated at all, because the
	   final byte of a complete multi-byte character is a continuation byte too. Knowing a
	   character's length before copying it removes the whole class.

	   Dropped characters are dropped SILENTLY on purpose: the alert is on screen in front of an
	   audience, and a name that renders as a name is the right outcome. The count returned is
	   how many were removed, for the log. */
	size_t removed = 0, j = 0;
	if (!out || cap == 0)
		return 0;
	out[0] = '\0';
	for (size_t i = 0; in && in[i];) {
		unsigned char c = (unsigned char)in[i];
		size_t len = 1;
		if ((c & 0x80) == 0)
			len = 1;
		else if ((c & 0xe0) == 0xc0)
			len = 2;
		else if ((c & 0xf0) == 0xe0)
			len = 3;
		else if ((c & 0xf8) == 0xf0)
			len = 4;
		else {
			/* a stray continuation byte or an invalid lead: malformed input, not a
			   character. Drop the one byte and carry on rather than guessing a length
			   and walking off the end of a sequence that was never there. */
			removed++;
			i++;
			continue;
		}
		/* a multi-byte sequence cut short by the end of the input is malformed the same way */
		for (size_t k = 1; k < len; k++) {
			if (((unsigned char)in[i + k] & 0xc0) != 0x80) {
				len = 0;
				break;
			}
		}
		if (len == 0) {
			removed++;
			i++;
			continue;
		}

		if (len == 1 && (c < 0x20 || c == 0x7f)) { /* C0 and DEL, newline and tab included */
			removed++;
			i++;
			continue;
		}
		if (len == 2 || len == 3) {
			uint32_t cp = len == 2 ? (uint32_t)(c & 0x1f) << 6 | ((unsigned char)in[i + 1] & 0x3f)
					       : (uint32_t)(c & 0x0f) << 12 |
							 (uint32_t)((unsigned char)in[i + 1] & 0x3f) << 6 |
							 ((unsigned char)in[i + 2] & 0x3f);
			/* U+202A..U+202E embeddings and overrides, U+2066..U+2069 isolates,
			   U+200E/U+200F and U+061C marks. U+2022 BULLET and the rest of the block
			   are ordinary characters and must survive. */
			bool bidi = (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200e ||
				    cp == 0x200f || cp == 0x061c;
			if (bidi) {
				removed++;
				i += len;
				continue;
			}
		}

		/* the whole character or none of it: a partial sequence draws as a replacement glyph
		   on one text source and is refused outright by another */
		if (j + len + 1 > cap)
			break;
		memcpy(out + j, in + i, len);
		j += len;
		i += len;
	}
	out[j] = '\0';
	return removed;
}
