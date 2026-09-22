/* A username is hostile input painted on a live stream.
 *
 * Why this file exists: everything else in the alert path is checked by rendering a frame, and a
 * frame cannot tell you that a name was SAFE -- a right-to-left override renders as a perfectly
 * ordinary-looking alert that says something the sender did not type, and a 4000-character name
 * renders as a frame that is simply full. Both look fine to a pixel gate.
 *
 * ARMED by mutation, because "I wrote a test" is not evidence it can fail. Each guard in
 * ff_alert_sanitise was removed in turn and this file recompiled:
 *
 *     control (every guard in place)   45 checks,  0 failed
 *     control characters removed       45 checks,  6 failed
 *     bidi override check removed      45 checks, 10 failed
 *     whole-character fit removed      45 checks, 10 failed
 *     truncated-sequence check removed 45 checks,  1 failed
 *     invalid-lead-byte check removed  45 checks,  6 failed
 *
 * The controls near the top -- ordinary names, including Japanese and an emoji -- pass in every
 * one of those mutants, so a guard that simply refused everything would not sneak through either.
 */

#include "ff-test.h"
#include <ff-alert.h>
#include <ff-alert-queue.h>
#include <obs-module.h>
#include <string.h>

/* OBS_DECLARE_MODULE() defines this inside the plugin, not in libobs. ff_alert_sanitise touches
   nothing on the module, so returning NULL is safe here; ff_alert_text_kind() would need a real
   OBS and is deliberately not called. */
obs_module_t *obs_current_module(void)
{
	return NULL;
}

/* The queue is where a raid either works or loses people.
 *
 * Alerts arrive in bursts and each takes seconds to play, so "show it now" is never the whole
 * story -- without a queue the second event of a raid overwrites the first while it is still on
 * screen, and everyone but the last person to arrive is simply never shown. None of that is
 * visible in a frame: the alert that IS drawn looks perfect.
 *
 * ARMED: numbers in the commit that introduced it.
 */
static void check_queue(void)
{
	struct ff_alert_queue q;
	struct ff_alert_event e, got;

	ff_alert_queue_init(&q);
	CHECK(q.count == 0 && q.dropped == 0);
	CHECK(!ff_alert_queue_pop(&q, &got)); /* empty pops report empty, they do not invent an event */

	/* FIFO: the person who arrived first is shown first. */
	for (int i = 0; i < 5; i++) {
		memset(&e, 0, sizeof e);
		e.kind = FF_ALERT_FOLLOW;
		snprintf(e.name, sizeof e.name, "viewer%d", i);
		CHECK(ff_alert_queue_push(&q, &e));
	}
	CHECK(q.count == 5);
	for (int i = 0; i < 5; i++) {
		char want[32];
		snprintf(want, sizeof want, "viewer%d", i);
		CHECK(ff_alert_queue_pop(&q, &got));
		CHECK(!strcmp(got.name, want));
	}
	CHECK(q.count == 0);
	CHECK(!ff_alert_queue_pop(&q, &got));

	/* The event is COPIED in, not referenced: whatever produced it is long gone by the time it
	   is drawn. */
	memset(&e, 0, sizeof e);
	e.kind = FF_ALERT_BITS;
	e.amount = 500;
	e.tier = 3;
	snprintf(e.name, sizeof e.name, "generous");
	snprintf(e.message, sizeof e.message, "have some bits");
	CHECK(ff_alert_queue_push(&q, &e));
	memset(&e, 0xAB, sizeof e); /* scribble over the caller's copy */
	CHECK(ff_alert_queue_pop(&q, &got));
	CHECK(got.kind == FF_ALERT_BITS && got.amount == 500 && got.tier == 3);
	CHECK(!strcmp(got.name, "generous") && !strcmp(got.message, "have some bits"));

	/* Overflow drops the NEWEST and says how many. The people already waiting asked first;
	   discarding the front would play a raid out of order with its earliest supporters gone. */
	ff_alert_queue_init(&q);
	for (int i = 0; i < FF_ALERT_QUEUE_MAX; i++) {
		memset(&e, 0, sizeof e);
		snprintf(e.name, sizeof e.name, "n%d", i);
		CHECK(ff_alert_queue_push(&q, &e));
	}
	memset(&e, 0, sizeof e);
	snprintf(e.name, sizeof e.name, "overflow");
	CHECK(!ff_alert_queue_push(&q, &e));
	CHECK(q.dropped == 1);
	CHECK(q.count == FF_ALERT_QUEUE_MAX);
	CHECK(ff_alert_queue_pop(&q, &got));
	CHECK(!strcmp(got.name, "n0")); /* the oldest survived; the newest was the one refused */

	/* The ring wraps without losing order -- the bug a plain array with a moving tail hides
	   until exactly the capacity-th event. */
	ff_alert_queue_init(&q);
	for (int round = 0; round < 3; round++) {
		for (int i = 0; i < FF_ALERT_QUEUE_MAX; i++) {
			memset(&e, 0, sizeof e);
			snprintf(e.name, sizeof e.name, "r%dn%d", round, i);
			CHECK(ff_alert_queue_push(&q, &e));
		}
		for (int i = 0; i < FF_ALERT_QUEUE_MAX; i++) {
			char want[32];
			snprintf(want, sizeof want, "r%dn%d", round, i);
			CHECK(ff_alert_queue_pop(&q, &got));
			CHECK(!strcmp(got.name, want));
		}
	}

	/* Clearing says how many it threw away, for the streamer thirty alerts deep in a raid. */
	ff_alert_queue_init(&q);
	for (int i = 0; i < 7; i++) {
		memset(&e, 0, sizeof e);
		CHECK(ff_alert_queue_push(&q, &e));
	}
	CHECK(ff_alert_queue_clear(&q) == 7);
	CHECK(q.count == 0);
	CHECK(ff_alert_queue_clear(&q) == 0);
	/* a cleared queue is still usable, not wedged */
	CHECK(ff_alert_queue_push(&q, &e));
	CHECK(ff_alert_queue_pop(&q, &got));

	/* NULLs are refused rather than dereferenced: these come from an event feed. */
	CHECK(!ff_alert_queue_push(NULL, &e));
	CHECK(!ff_alert_queue_push(&q, NULL));
	CHECK(!ff_alert_queue_pop(NULL, &got));
	CHECK(!ff_alert_queue_pop(&q, NULL));
	CHECK(ff_alert_queue_clear(NULL) == 0);

	/* Kind ids round-trip, and an unknown one is REFUSED rather than defaulting -- a kind that
	   silently became "follow" would show the wrong alert for a real event. */
	for (int i = 0; i < FF_ALERT_KIND_COUNT; i++) {
		enum ff_alert_kind k;
		const char *id = ff_alert_kind_id((enum ff_alert_kind)i);
		CHECK(strcmp(id, "unknown") != 0); /* every declared kind has a real id */
		CHECK(ff_alert_kind_parse(id, &k));
		CHECK(k == (enum ff_alert_kind)i);
	}
	{
		enum ff_alert_kind k = FF_ALERT_RAID;
		CHECK(!ff_alert_kind_parse("nonsense", &k));
		CHECK(k == FF_ALERT_RAID); /* left alone on failure */
		CHECK(!ff_alert_kind_parse("", &k));
		CHECK(!ff_alert_kind_parse(NULL, &k));
		CHECK(!ff_alert_kind_parse("FOLLOW", &k)); /* ids are lower case, exactly */
	}
	CHECK(!strcmp(ff_alert_kind_id((enum ff_alert_kind)999), "unknown"));
	CHECK(!strcmp(ff_alert_kind_id((enum ff_alert_kind) - 1), "unknown"));
}

static const char *san(const char *in, char *buf, size_t cap, size_t *removed)
{
	size_t r = ff_alert_sanitise(in, buf, cap);
	if (removed)
		*removed = r;
	return buf;
}

int main(void)
{
	char b[64];
	size_t removed;

	/* Controls: ordinary names must survive untouched, or every check below could be passing
	   because the function refuses everything. */
	CHECK(!strcmp(san("NightbotFan42", b, sizeof b, &removed), "NightbotFan42"));
	CHECK(removed == 0);
	CHECK(!strcmp(san("Ünicode_Näme", b, sizeof b, &removed), "Ünicode_Näme"));
	CHECK(removed == 0);
	CHECK(!strcmp(san("日本語のなまえ", b, sizeof b, &removed), "日本語のなまえ"));
	CHECK(removed == 0);
	CHECK(!strcmp(san("", b, sizeof b, &removed), ""));
	CHECK(ff_alert_sanitise(NULL, b, sizeof b) == 0 && b[0] == '\0');

	/* Control characters. A newline in a name turns a one-line alert into a two-line one and
	   pushes the rest of the layout off; a NUL would truncate it silently. */
	CHECK(!strcmp(san("bad\nname", b, sizeof b, &removed), "badname"));
	CHECK(removed == 1);
	CHECK(!strcmp(san("tab\there", b, sizeof b, &removed), "tabhere"));
	CHECK(removed == 1);
	CHECK(!strcmp(san("bell\x07ring\x7f", b, sizeof b, &removed), "bellring"));
	CHECK(removed == 2);

	/* Bidirectional overrides. U+202E (RIGHT-TO-LEFT OVERRIDE) reverses everything after it,
	   so a name of "gift" + U+202E + "knarp" draws as "giftprank" -- a word the sender never
	   typed, on someone else's stream.

	   The character is written as escaped bytes below and NOT as a literal, deliberately: gcc
	   refuses a source file containing an unpaired bidi control with -Werror=bidi-chars, which
	   is the same attack one level up (CVE-2021-42574, "Trojan Source" -- code that reads one
	   way and compiles another). A test for the hazard must not carry the hazard. */
	CHECK(!strcmp(san("gift\xe2\x80\xaeknarp", b, sizeof b, &removed), "giftknarp"));
	CHECK(removed == 1);
	CHECK(!strcmp(san("\xe2\x80\xaa\xe2\x80\xab\xe2\x80\xac\xe2\x80\xad\xe2\x80\xae"
			  "ok",
			  b, sizeof b, &removed),
		      "ok")); /* LRE RLE PDF LRO RLO */
	CHECK(removed == 5);
	CHECK(!strcmp(san("\xe2\x81\xa6\xe2\x81\xa7\xe2\x81\xa8\xe2\x81\xa9"
			  "ok",
			  b, sizeof b, &removed),
		      "ok")); /* LRI RLI FSI PDI */
	CHECK(removed == 4);
	/* a legitimate U+2022 BULLET starts e2 80 too, and must NOT be eaten */
	CHECK(!strcmp(san("a\xe2\x80\xa2"
			  "b",
			  b, sizeof b, &removed),
		      "a\xe2\x80\xa2"
		      "b"));
	CHECK(removed == 0);

	/* Length, and the part a naive truncation gets wrong: cutting mid-sequence leaves a partial
	   UTF-8 character that a text source draws as a replacement glyph or refuses outright. */
	char small[8];
	CHECK(!strcmp(san("abcdefghijkl", small, sizeof small, &removed), "abcdefg"));
	/* "日本語" is 3 bytes each: 7 bytes of room holds two whole characters, never two and a
	   third of one */
	CHECK(!strcmp(san("日本語", small, sizeof small, &removed), "日本"));
	CHECK(strlen(san("日本語", small, sizeof small, &removed)) == 6);
	/* the same cut one byte earlier, to prove the backoff is not an off-by-one that happens to
	   land well for this one string */
	char small7[7];
	CHECK(!strcmp(san("日本語", small7, sizeof small7, &removed), "日本"));
	char small6[6];
	CHECK(!strcmp(san("日本語", small6, sizeof small6, &removed), "日"));
	char small3[3];
	CHECK(!strcmp(san("日本語", small3, sizeof small3, &removed), ""));

	/* Malformed UTF-8 must not walk off the end of a sequence that was never there. A lead
	   byte announcing three bytes with only one following it is exactly what a truncated name
	   from the wire looks like. */
	CHECK(!strcmp(san("ok\xe6\x97", b, sizeof b, &removed), "ok"));
	CHECK(removed == 2); /* the lead and the orphaned continuation, one at a time */
	CHECK(!strcmp(san("\x80\x80ok", b, sizeof b, &removed), "ok"));
	CHECK(removed == 2);
	CHECK(!strcmp(san("\xff\xfeok", b, sizeof b, &removed), "ok"));
	CHECK(removed == 2);
	/* a 4-byte character (an emoji) is a character, not three stray bytes */
	CHECK(!strcmp(san("a\xf0\x9f\xa6\x8a"
			  "b",
			  b, sizeof b, &removed),
		      "a\xf0\x9f\xa6\x8a"
		      "b"));
	CHECK(removed == 0);
	/* and it is kept whole or dropped whole when the buffer runs out */
	char five[5];
	CHECK(!strcmp(san("\xf0\x9f\xa6\x8a"
			  "xy",
			  five, sizeof five, &removed),
		      "\xf0\x9f\xa6\x8a"));
	/* When the FIRST character does not fit, the result is empty -- truncation stops, it does
	   not skip ahead to characters that would. Skipping would silently change the name (the
	   emoji vanishing from the front of "<fox>xy" to leave "xy") rather than shorten it, and a
	   shortened name is a name while a reordered one is somebody else's. */
	char four[4];
	CHECK(!strcmp(san("\xf0\x9f\xa6\x8a"
			  "xy",
			  four, sizeof four, &removed),
		      ""));
	CHECK(!strcmp(san("ab\xf0\x9f\xa6\x8a", four, sizeof four, &removed), "ab"));

	/* U+200E LEFT-TO-RIGHT MARK and U+061C ARABIC LETTER MARK: not overrides, but they still
	   reorder what is drawn around them. */
	CHECK(!strcmp(san("a\xe2\x80\x8e"
			  "b",
			  b, sizeof b, &removed),
		      "ab"));
	CHECK(removed == 1);
	CHECK(!strcmp(san("a\xd8\x9c"
			  "b",
			  b, sizeof b, &removed),
		      "ab"));
	CHECK(removed == 1);

	/* A one-byte buffer has room for the terminator and nothing else. */
	char one[1];
	CHECK(ff_alert_sanitise("anything", one, sizeof one) == 0 && one[0] == '\0');
	CHECK(ff_alert_sanitise("anything", b, 0) == 0);

	check_queue();
	FF_TEST_MAIN_END();
}
