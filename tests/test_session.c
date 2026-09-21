/* The session's decisions, driven with scripted messages and a clock this file invents.
 *
 * Every one of these was previously made inline inside a loop that needed a Twitch account, a
 * live channel and Twitch choosing to redeploy before it could be exercised. A review found four
 * defects in that loop. This is the case that matters most:
 *
 *   A session_reconnect is ROUTINE -- Twitch sends it for maintenance. The subscriptions move to
 *   the new session automatically. Asking for them again returns 409 Conflict for every type,
 *   which the old code counted as a refusal, concluded "Twitch refused every alert type", and
 *   gave up on for the rest of the process. Alerts were dead until the streamer noticed, and the
 *   message sent them to re-audit permissions that were never the problem.
 *
 * ARMED by mutation -- each decision inverted in turn, recompiled and rerun:
 *
 *     control (every guard in place)                 45 checks,  0 failed
 *     re-subscribes after a reconnect                45 checks,  2 failed
 *     `subscribed` reset when a socket opens         45 checks,  3 failed
 *     reconnect URL not taken                        45 checks,  1 failed
 *     no welcome deadline                            45 checks,  2 failed
 *     keepalive watchdog removed                     45 checks,  3 failed
 *     a message does not reset the clock             45 checks,  1 failed
 *     revocation drops the connection                45 checks,  1 failed
 *     an unreadable message drops it                 45 checks,  1 failed
 *     a partial subscribe drops the connection       45 checks,  2 failed
 *     zero accepted is treated as fine               45 checks,  2 failed
 *
 * One further mutation -- ff_session_opened() not resetting `last_message` -- changed no result,
 * and on inspection cannot: the first message always sets it, and before that the welcome
 * deadline is what bounds the connection. Left in as defensive, recorded as equivalent rather
 * than written a test around.
 *
 * This file also failed on its own first run, on a case where the TEST was wrong: it reused a
 * session that was already subscribed and expected a welcome to mean SUBSCRIBE. That is the
 * reconnect behaviour, and the state machine was right.
 */

#include "ff-test.h"
#include <ff-session.h>
#include <obs-module.h>
#include <string.h>

obs_module_t *obs_current_module(void)
{
	return NULL;
}

const char *obs_module_text(const char *key)
{
	return key;
}

#define WELCOME(ID, KA)                                                                   \
	"{\"metadata\":{\"message_type\":\"session_welcome\"},\"payload\":{\"session\":{"  \
	"\"id\":\"" ID "\",\"keepalive_timeout_seconds\":" KA ",\"reconnect_url\":null}}}"

#define RECONNECT(URL)                                                                       \
	"{\"metadata\":{\"message_type\":\"session_reconnect\"},\"payload\":{\"session\":{"   \
	"\"id\":\"s2\",\"keepalive_timeout_seconds\":10,\"reconnect_url\":\"" URL "\"}}}"

static const char KEEPALIVE[] =
	"{\"metadata\":{\"message_type\":\"session_keepalive\"},\"payload\":{}}";

static const char RAID[] =
	"{\"metadata\":{\"message_type\":\"notification\"},\"payload\":{\"subscription\":"
	"{\"type\":\"channel.raid\"},\"event\":{\"from_broadcaster_user_name\":\"Big\","
	"\"viewers\":50}}}";

static const char REVOKED[] =
	"{\"metadata\":{\"message_type\":\"revocation\"},\"payload\":{\"subscription\":"
	"{\"type\":\"channel.follow\",\"status\":\"authorization_revoked\"}}}";

int main(void)
{
	struct ff_session s;
	struct ff_alert_event ev;
	time_t now = 1000;

	/* ---- a first connection: welcome, subscribe, then events ---- */
	ff_session_init(&s, "wss://eventsub.wss.twitch.tv/ws");
	CHECK(strcmp(s.url, "wss://eventsub.wss.twitch.tv/ws") == 0);
	ff_session_opened(&s, now);

	CHECK(ff_session_message(&s, WELCOME("s1", "10"), now, &ev) == FF_STEP_SUBSCRIBE);
	CHECK(strcmp(s.es.session_id, "s1") == 0);
	CHECK(ff_session_subscribed(&s, 7, 7) == FF_STEP_NOTHING);
	CHECK(s.subscribed);

	now += 1;
	CHECK(ff_session_message(&s, RAID, now, &ev) == FF_STEP_EMIT);
	CHECK(ev.kind == FF_ALERT_RAID && ev.amount == 50);

	now += 1;
	CHECK(ff_session_message(&s, KEEPALIVE, now, &ev) == FF_STEP_NOTHING);

	/* ---- the reconnect, which is where this used to fall over ---- */
	now += 1;
	CHECK(ff_session_message(&s, RECONNECT("wss://eventsub.wss.twitch.tv/ws?c=2"), now, &ev) ==
	      FF_STEP_RECONNECT);
	CHECK(strcmp(s.url, "wss://eventsub.wss.twitch.tv/ws?c=2") == 0);
	CHECK(s.subscribed); /* it must SURVIVE the reconnect */

	/* the new socket comes up and welcomes us again */
	now += 2;
	ff_session_opened(&s, now);
	CHECK(s.subscribed); /* opening a socket must not forget it either */
	enum ff_session_step step = ff_session_message(&s, WELCOME("s2", "10"), now, &ev);
	CHECK(step != FF_STEP_SUBSCRIBE);
	CHECK(step == FF_STEP_NOTE);
	if (step == FF_STEP_SUBSCRIBE)
		fprintf(stderr, "      (re-subscribing after a reconnect: Twitch answers 409 to "
				"every type, which reads as 'refused every alert type')\n");
	/* and the connection keeps working afterwards */
	now += 1;
	CHECK(ff_session_message(&s, RAID, now, &ev) == FF_STEP_EMIT);

	/* ---- the watchdog ---- */
	ff_session_init(&s, "wss://x/ws");
	ff_session_opened(&s, 1000);
	CHECK(ff_session_message(&s, WELCOME("s1", "10"), 1000, &ev) == FF_STEP_SUBSCRIBE);
	CHECK(ff_session_subscribed(&s, 7, 7) == FF_STEP_NOTHING);

	CHECK(ff_session_idle(&s, 1005) == FF_STEP_NOTHING);  /* well inside */
	CHECK(ff_session_idle(&s, 1020) == FF_STEP_NOTHING);  /* one missed keepalive is allowed */
	CHECK(ff_session_idle(&s, 1023) == FF_STEP_DROP);     /* past that it is gone */
	CHECK(strstr(s.reason, "quiet") != NULL);

	/* a keepalive resets it. A FRESH session, not the one above: that one is already
	   subscribed, so its next welcome is correctly a NOTE rather than a SUBSCRIBE -- which is
	   the reconnect behaviour, and reusing it here made this file fail on its own first run. */
	ff_session_init(&s, "wss://x/ws");
	ff_session_opened(&s, 2000);
	CHECK(ff_session_message(&s, WELCOME("s1", "10"), 2000, &ev) == FF_STEP_SUBSCRIBE);
	CHECK(ff_session_subscribed(&s, 7, 7) == FF_STEP_NOTHING);
	CHECK(ff_session_idle(&s, 2020) == FF_STEP_NOTHING);
	CHECK(ff_session_message(&s, KEEPALIVE, 2020, &ev) == FF_STEP_NOTHING);
	CHECK(ff_session_idle(&s, 2035) == FF_STEP_NOTHING); /* late, but counted from 2020 */
	CHECK(ff_session_idle(&s, 2045) == FF_STEP_DROP);

	/* ---- a socket that opens and then says nothing ----
	   Before the welcome there is no agreed keepalive, so the watchdog above cannot fire. This
	   is the only bound on that case: without it the connection sits on "Connecting" forever,
	   with no error and no retry. */
	ff_session_init(&s, "wss://x/ws");
	ff_session_opened(&s, 5000);
	CHECK(ff_session_idle(&s, 5000 + FF_SESSION_WELCOME_SECS - 1) == FF_STEP_NOTHING);
	CHECK(ff_session_idle(&s, 5000 + FF_SESSION_WELCOME_SECS + 1) == FF_STEP_DROP);
	CHECK(strstr(s.reason, "never started the session") != NULL);

	/* ---- what the subscribe step reports ---- */
	ff_session_init(&s, "wss://x/ws");
	ff_session_opened(&s, 6000);
	CHECK(ff_session_message(&s, WELCOME("s1", "10"), 6000, &ev) == FF_STEP_SUBSCRIBE);
	/* none accepted: end the connection and retry -- but NOT permanently, and that is the
	   caller's business, not this file's */
	CHECK(ff_session_subscribed(&s, 0, 7) == FF_STEP_DROP);
	CHECK(strstr(s.reason, "none") != NULL);

	ff_session_init(&s, "wss://x/ws");
	ff_session_opened(&s, 6000);
	CHECK(ff_session_message(&s, WELCOME("s1", "10"), 6000, &ev) == FF_STEP_SUBSCRIBE);
	/* some accepted: say so and carry on. One missing scope takes out one alert type; the
	   other six still work, and dropping the connection would lose those too. */
	CHECK(ff_session_subscribed(&s, 6, 7) == FF_STEP_NOTE);
	CHECK(strstr(s.reason, "6 of 7") != NULL);

	/* ---- a revocation is ONE subscription, not the connection ---- */
	now = 7000;
	ff_session_init(&s, "wss://x/ws");
	ff_session_opened(&s, now);
	CHECK(ff_session_message(&s, WELCOME("s1", "10"), now, &ev) == FF_STEP_SUBSCRIBE);
	CHECK(ff_session_subscribed(&s, 7, 7) == FF_STEP_NOTHING);
	CHECK(ff_session_message(&s, REVOKED, now, &ev) == FF_STEP_NOTE);
	CHECK(strstr(s.reason, "channel.follow") != NULL);
	/* and everything else still arrives */
	CHECK(ff_session_message(&s, RAID, now, &ev) == FF_STEP_EMIT);

	/* ---- a message we cannot read is not a reason to drop a working connection ----
	   Twitch can add a message type or a field whenever it likes. */
	CHECK(ff_session_message(&s, "{ not json", now, &ev) == FF_STEP_NOTE);
	CHECK(ff_session_message(&s, RAID, now, &ev) == FF_STEP_EMIT);

	/* a gifted sub's duplicate channel.subscribe is dropped, with a note saying why */
	static const char GIFTSUB[] =
		"{\"metadata\":{\"message_type\":\"notification\"},\"payload\":{\"subscription\":"
		"{\"type\":\"channel.subscribe\"},\"event\":{\"user_name\":\"N\",\"tier\":\"1000\","
		"\"is_gift\":true}}}";
	CHECK(ff_session_message(&s, GIFTSUB, now, &ev) == FF_STEP_NOTE);
	CHECK(strstr(s.reason, "gift") != NULL);

	FF_TEST_MAIN_END();
}
