/* The two decisions that decide whether alerts survive a bad hour.
 *
 * Neither is reproducible by clicking: the failure is "alerts quietly stopped forty minutes into
 * the stream", and by the time anyone notices, the cause is gone. So both are pure functions and
 * both are pinned here.
 *
 * ARMED by mutation -- table at the end, measured.
 */

#include "ff-test.h"
#include <ff-twitch.h>
#include <string.h>
#include <obs-module.h>

/* OBS_DECLARE_MODULE() defines these inside the plugin. Neither function under test touches
   either one; they are here so the rest of ff-twitch.c links. */
obs_module_t *obs_current_module(void)
{
	return NULL;
}

const char *obs_module_text(const char *key)
{
	return key;
}

int main(void)
{
	/* ---- backoff ---- */
	CHECK(ff_twitch_backoff_secs(1) == 1);
	CHECK(ff_twitch_backoff_secs(2) == 2);
	CHECK(ff_twitch_backoff_secs(3) == 4);
	CHECK(ff_twitch_backoff_secs(4) == 8);
	CHECK(ff_twitch_backoff_secs(5) == 16);
	CHECK(ff_twitch_backoff_secs(6) == 32);

	/* Capped. A stream that gets its alerts back after ten minutes of silence has already lost
	   the moment; unbounded doubling reaches hours by the twelfth attempt. */
	CHECK(ff_twitch_backoff_secs(7) == 60);
	CHECK(ff_twitch_backoff_secs(20) == 60);
	CHECK(ff_twitch_backoff_secs(1000) == 60);

	/* It never returns 0 or a negative, whatever it is handed: a zero delay is a reconnect
	   loop at full speed, which is how a client earns a rate limit and then looks broken for
	   reasons that have nothing to do with the original fault. */
	for (int i = -5; i < 40; i++)
		CHECK(ff_twitch_backoff_secs(i) >= 1 && ff_twitch_backoff_secs(i) <= 60);

	/* and it only ever grows */
	for (int i = 1; i < 30; i++)
		CHECK(ff_twitch_backoff_secs(i + 1) >= ff_twitch_backoff_secs(i));

	/* ---- the keepalive watchdog ---- */
	const time_t now = 1000000;

	/* a keepalive just arrived */
	CHECK(!ff_twitch_is_stale(now, 10, now));
	CHECK(!ff_twitch_is_stale(now - 5, 10, now));

	/* ONE missed keepalive is not a fault. A machine mid-encode can be late, and reconnecting
	   for that drops a working connection and re-subscribes seven times for nothing. */
	CHECK(!ff_twitch_is_stale(now - 15, 10, now));
	CHECK(!ff_twitch_is_stale(now - 22, 10, now));

	/* past that, the connection is open at the socket and dead at the service -- which is
	   exactly what a dropped wifi link looks like from in here, and the only way to see it */
	CHECK(ff_twitch_is_stale(now - 23, 10, now));
	CHECK(ff_twitch_is_stale(now - 600, 10, now));

	/* Twitch may pick anything from 10 to 600 seconds, and the window has to follow it */
	CHECK(!ff_twitch_is_stale(now - 400, 600, now));
	CHECK(ff_twitch_is_stale(now - 1300, 600, now));
	CHECK(!ff_twitch_is_stale(now - 100, 60, now));
	CHECK(ff_twitch_is_stale(now - 200, 60, now));

	/* A keepalive interval we never received is not a reason to invent one. Zero here would
	   make every connection instantly stale and reconnect forever -- a bug that would look
	   like Twitch being unreliable. */
	CHECK(!ff_twitch_is_stale(now - 100000, 0, now));
	CHECK(!ff_twitch_is_stale(now - 100000, -1, now));

	/* nothing has arrived yet: not stale, because nothing has had a chance to be late */
	CHECK(!ff_twitch_is_stale(0, 10, now));

	/* a clock that went backwards must not fire the watchdog either */
	CHECK(!ff_twitch_is_stale(now + 500, 10, now));

	/* ---- the locale placeholders, which are NOT printf ----
	 *
	 * obs_module_text is stubbed above to return the key, so these drive ff_twitch_format with
	 * the real strings rather than the real table. The point is the substitution mechanism: the
	 * version that shipped handed these to vsnprintf, where "%1s" is width-1 "%s" and an int
	 * argument is dereferenced as a pointer. It segfaulted on the ordinary reconnect path. */
	char line[512];

	ff_twitch_format("Reconnecting in %1s", "8", NULL, NULL, line, sizeof line);
	CHECK(strcmp(line, "Reconnecting in 8s") == 0);

	ff_twitch_format("Go to %2 and enter the code %1", "ABCD1234", "twitch.tv/activate", NULL,
			 line, sizeof line);
	CHECK(strcmp(line, "Go to twitch.tv/activate and enter the code ABCD1234") == 0);

	/* three of them, and %3 is not confused with %1 followed by a 3 */
	ff_twitch_format("%3: %1 of %2", "6", "7", "junkie", line, sizeof line);
	CHECK(strcmp(line, "junkie: 6 of 7") == 0);

	/* A VALUE containing a placeholder must not be substituted again. A display name is
	   whatever someone typed, and "%2" is a perfectly ordinary thing to type. */
	ff_twitch_format("%1 and %2", "%2", "second", NULL, line, sizeof line);
	CHECK(strcmp(line, "%2 and second") == 0);

	/* a string with no placeholders is left exactly alone */
	ff_twitch_format("Not connected to Twitch", NULL, NULL, NULL, line, sizeof line);
	CHECK(strcmp(line, "Not connected to Twitch") == 0);

	/* an argument for a placeholder that is not there changes nothing */
	ff_twitch_format("No placeholders", "x", "y", "z", line, sizeof line);
	CHECK(strcmp(line, "No placeholders") == 0);

	FF_TEST_MAIN_END();
}

/* Measured, each guard removed in turn, recompiled and rerun:
 *
 *     control (every guard in place)          103 checks,  0 failed
 *     backoff cap removed                     103 checks, 36 failed
 *     one-missed-keepalive allowance removed  103 checks,  3 failed
 *     keepalive <= 0 guard removed            103 checks,  2 failed
 *     last_message <= 0 guard removed         103 checks,  1 failed
 *     placeholder substitution order reversed 103 checks,  1 failed
 *
 * A sixth mutation -- removing the `attempt < 1` floor from the backoff -- changed no result at
 * all. That is not a gap in this file: the loop does not run for anything <= 1, so 0 and
 * negatives already returned 1 and the guard was dead code. It was deleted rather than tested,
 * because a test written to justify a line that does nothing is a test that proves nothing.
 */
