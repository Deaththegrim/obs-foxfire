/* EventSub messages, turned into alerts.
 *
 * Every message here is a whole envelope in the shape Twitch actually sends, because the failures
 * this file exists to catch are all "it parsed, and the answer was wrong":
 *
 *   - `tier` arrives as the STRING "2000". obs_data_get_int() on a string returns 0 with no error,
 *     so reading it as a number makes every subscriber Prime and nothing anywhere complains.
 *   - `message` is a plain string on a cheer and an OBJECT with a "text" member on a resub. One
 *     reading gives empty resub messages forever.
 *   - a gifted sub fires channel.subscribe AND channel.subscription.gift. Counting both turns one
 *     gift of fifty into fifty-one alerts.
 *   - a raid has no `user_name` at all; the raider is `from_broadcaster_user_name`.
 *   - an anonymous gifter's name is empty, and "  just gifted 5 subs" is what trusting it looks
 *     like on someone's stream.
 *
 * ARMED by mutation -- each guard removed in turn, recompiled and rerun. Where removing one
 * outright leaves a variable unused (-Werror refuses that), it was disabled with `&& false`:
 *
 *     control (every guard in place)                 90 checks,  0 failed
 *     tier read as a number, not a string            90 checks,  1 failed
 *     message object fallback removed                90 checks,  1 failed
 *     is_gift check removed (gifts counted twice)    90 checks,  2 failed
 *     raid raider read from user_name                90 checks,  1 failed
 *     anonymous fallback removed                     90 checks,  2 failed
 *     gift total floor removed                       90 checks,  1 failed
 *     reconnect with no URL accepted                 90 checks,  1 failed
 *     welcome with no session id accepted            90 checks,  2 failed
 *     reward title fallback removed                  90 checks,  1 failed
 *     unknown subscription type handled anyway       90 checks,  1 failed
 *     notification with no event accepted            90 checks,  1 failed
 *
 * "welcome with no session id" SURVIVED the first time round, at 0 failed. The only welcome this
 * file refused was one with no session object at all, which a different branch catches; a session
 * that is present and empty went straight through, and the client would have subscribed against
 * an empty session id and then waited forever for events that go nowhere. The case was added.
 */

#include "ff-test.h"
#include <ff-eventsub.h>
#include <obs-module.h>
#include <string.h>

/* OBS_DECLARE_MODULE() defines this inside the plugin. Nothing here needs a loaded module: the
   only call that consults it is obs_module_text(), and libobs returns the key itself when there
   is no lookup table, which is why the anonymous checks below look for the KEY. */
obs_module_t *obs_current_module(void)
{
	return NULL;
}

const char *obs_module_text(const char *key)
{
	return key;
}

static enum ff_es_result handle(struct ff_es *s, const char *json, struct ff_alert_event *e)
{
	memset(e, 0, sizeof *e);
	return ff_es_handle(s, json, e);
}

#define NOTIF(TYPE, EVENT)                                                      \
	"{\"metadata\":{\"message_id\":\"m1\",\"message_type\":\"notification\"," \
	"\"subscription_type\":\"" TYPE "\",\"subscription_version\":\"1\"},"     \
	"\"payload\":{\"subscription\":{\"id\":\"s1\",\"type\":\"" TYPE "\","     \
	"\"status\":\"enabled\"},\"event\":" EVENT "}}"

int main(void)
{
	struct ff_es s;
	memset(&s, 0, sizeof s);
	struct ff_alert_event e;

	/* ---- the session ---- */
	static const char WELCOME[] = "{\"metadata\":{\"message_id\":\"m0\",\"message_type\":\"session_welcome\"},"
				      "\"payload\":{\"session\":{\"id\":\"AgoQ1234\",\"status\":\"connected\","
				      "\"keepalive_timeout_seconds\":10,\"reconnect_url\":null}}}";
	CHECK(handle(&s, WELCOME, &e) == FF_ES_WELCOME);
	CHECK(strcmp(s.session_id, "AgoQ1234") == 0);
	CHECK(s.keepalive_secs == 10);

	/* A welcome carrying a session with no id. Not the same as a welcome with no session at
	   all, which the branch above catches -- this one has the right shape and nothing usable
	   in it, and accepting it would have the client subscribe against an empty session id and
	   then wait forever for events that go nowhere. A surviving mutant found this missing. */
	static const char WELCOME_NOID[] = "{\"metadata\":{\"message_type\":\"session_welcome\"},"
					   "\"payload\":{\"session\":{\"id\":\"\",\"keepalive_timeout_seconds\":10}}}";
	CHECK(handle(&s, WELCOME_NOID, &e) == FF_ES_BAD);
	CHECK(strstr(s.err, "session id") != NULL);

	static const char KEEPALIVE[] = "{\"metadata\":{\"message_id\":\"m2\",\"message_type\":\"session_keepalive\"},"
					"\"payload\":{}}";
	CHECK(handle(&s, KEEPALIVE, &e) == FF_ES_KEEPALIVE);

	static const char RECONNECT[] = "{\"metadata\":{\"message_id\":\"m3\",\"message_type\":\"session_reconnect\"},"
					"\"payload\":{\"session\":{\"id\":\"AgoQ1234\",\"status\":\"reconnecting\","
					"\"keepalive_timeout_seconds\":null,"
					"\"reconnect_url\":\"wss://eventsub.wss.twitch.tv/ws?challenge=abc\"}}}";
	CHECK(handle(&s, RECONNECT, &e) == FF_ES_RECONNECT);
	CHECK(strcmp(s.reconnect_url, "wss://eventsub.wss.twitch.tv/ws?challenge=abc") == 0);

	/* a reconnect we cannot act on must be an error, not a silent no-op that leaves the client
	   sitting on a connection Twitch is about to close */
	static const char RECONNECT_NOURL[] = "{\"metadata\":{\"message_type\":\"session_reconnect\"},"
					      "\"payload\":{\"session\":{\"id\":\"x\",\"reconnect_url\":\"\"}}}";
	CHECK(handle(&s, RECONNECT_NOURL, &e) == FF_ES_BAD);

	static const char REVOKED[] = "{\"metadata\":{\"message_type\":\"revocation\"},"
				      "\"payload\":{\"subscription\":{\"type\":\"channel.follow\","
				      "\"status\":\"authorization_revoked\"}}}";
	CHECK(handle(&s, REVOKED, &e) == FF_ES_REVOKED);
	CHECK(strstr(s.note, "channel.follow") != NULL);
	CHECK(strstr(s.note, "authorization_revoked") != NULL);

	/* ---- follow ---- */
	CHECK(handle(&s,
		     NOTIF("channel.follow", "{\"user_id\":\"1\",\"user_name\":\"Cassie\","
					     "\"broadcaster_user_name\":\"junkie\"}"),
		     &e) == FF_ES_EVENT);
	CHECK(e.kind == FF_ALERT_FOLLOW);
	CHECK(strcmp(e.name, "Cassie") == 0);

	/* ---- a new sub, tier 2 ---- */
	CHECK(handle(&s, NOTIF("channel.subscribe", "{\"user_name\":\"Mira\",\"tier\":\"2000\",\"is_gift\":false}"),
		     &e) == FF_ES_EVENT);
	CHECK(e.kind == FF_ALERT_SUB);
	CHECK(strcmp(e.name, "Mira") == 0);
	CHECK(e.tier == 2); /* the string "2000", not 0 and not 2000 */

	/* Prime is not a tier number, and must not become one */
	CHECK(handle(&s, NOTIF("channel.subscribe", "{\"user_name\":\"Ash\",\"tier\":\"Prime\",\"is_gift\":false}"),
		     &e) == FF_ES_EVENT);
	CHECK(e.tier == 0);

	/* ---- the same sub when it was GIFTED: dropped, because the gift event covers it ---- */
	CHECK(handle(&s, NOTIF("channel.subscribe", "{\"user_name\":\"Nell\",\"tier\":\"1000\",\"is_gift\":true}"),
		     &e) == FF_ES_IGNORED);
	CHECK(strstr(s.note, "gift") != NULL);

	/* ---- the gift itself ---- */
	CHECK(handle(&s,
		     NOTIF("channel.subscription.gift", "{\"user_name\":\"Bea\",\"tier\":\"1000\",\"total\":5,"
							"\"cumulative_total\":40,\"is_anonymous\":false}"),
		     &e) == FF_ES_EVENT);
	CHECK(e.kind == FF_ALERT_GIFT);
	CHECK(strcmp(e.name, "Bea") == 0);
	CHECK(e.amount == 5);
	CHECK(e.tier == 1);

	/* an anonymous gifter has no name at all */
	CHECK(handle(&s,
		     NOTIF("channel.subscription.gift", "{\"user_name\":null,\"tier\":\"1000\",\"total\":2,"
							"\"is_anonymous\":true}"),
		     &e) == FF_ES_EVENT);
	CHECK(strcmp(e.name, "Foxfire.Alert.Anonymous") == 0);
	CHECK(e.amount == 2);

	/* a gift with no total is one sub, not zero */
	CHECK(handle(&s, NOTIF("channel.subscription.gift", "{\"user_name\":\"Sol\",\"tier\":\"1000\"}"), &e) ==
	      FF_ES_EVENT);
	CHECK(e.amount == 1);

	/* ---- a resub, whose message is an OBJECT ---- */
	CHECK(handle(&s,
		     NOTIF("channel.subscription.message", "{\"user_name\":\"Wren\",\"tier\":\"3000\","
							   "\"cumulative_months\":12,\"streak_months\":3,"
							   "\"duration_months\":1,"
							   "\"message\":{\"text\":\"a year already\",\"emotes\":[]}}"),
		     &e) == FF_ES_EVENT);
	CHECK(e.kind == FF_ALERT_RESUB);
	CHECK(strcmp(e.name, "Wren") == 0);
	CHECK(e.tier == 3);
	CHECK(e.amount == 12);
	CHECK(strcmp(e.message, "a year already") == 0);

	/* ---- bits, whose message is a plain string ---- */
	CHECK(handle(&s,
		     NOTIF("channel.cheer", "{\"user_name\":\"Iris\",\"bits\":500,"
					    "\"message\":\"cheer500 go on\",\"is_anonymous\":false}"),
		     &e) == FF_ES_EVENT);
	CHECK(e.kind == FF_ALERT_BITS);
	CHECK(e.amount == 500);
	CHECK(strcmp(e.message, "cheer500 go on") == 0);

	CHECK(handle(&s,
		     NOTIF("channel.cheer", "{\"user_name\":null,\"bits\":100,\"message\":\"\","
					    "\"is_anonymous\":true}"),
		     &e) == FF_ES_EVENT);
	CHECK(strcmp(e.name, "Foxfire.Alert.Anonymous") == 0);

	/* ---- a raid, which names the raider in a different field entirely ---- */
	CHECK(handle(&s,
		     NOTIF("channel.raid", "{\"from_broadcaster_user_name\":\"BigStreamer\","
					   "\"to_broadcaster_user_name\":\"junkie\",\"viewers\":247}"),
		     &e) == FF_ES_EVENT);
	CHECK(e.kind == FF_ALERT_RAID);
	CHECK(strcmp(e.name, "BigStreamer") == 0);
	CHECK(e.amount == 247);

	/* ---- a channel point redemption ---- */
	CHECK(handle(&s,
		     NOTIF("channel.channel_points_custom_reward_redemption.add",
			   "{\"user_name\":\"Pip\",\"user_input\":\"play the song\","
			   "\"reward\":{\"title\":\"Song request\",\"cost\":500}}"),
		     &e) == FF_ES_EVENT);
	CHECK(e.kind == FF_ALERT_REDEEM);
	CHECK(strcmp(e.name, "Pip") == 0);
	CHECK(strcmp(e.message, "play the song") == 0);

	/* a reward with no input falls back to the reward's own name, so the alert still says
	   what happened rather than naming someone and stopping */
	CHECK(handle(&s,
		     NOTIF("channel.channel_points_custom_reward_redemption.add",
			   "{\"user_name\":\"Pip\",\"user_input\":\"\","
			   "\"reward\":{\"title\":\"Hydrate!\",\"cost\":100}}"),
		     &e) == FF_ES_EVENT);
	CHECK(strcmp(e.message, "Hydrate!") == 0);

	/* ---- malformed input is refused, and each refusal says which thing was missing ---- */
	CHECK(handle(&s, "not json at all", &e) == FF_ES_BAD);
	CHECK(handle(&s, "{\"payload\":{}}", &e) == FF_ES_BAD);                 /* no metadata */
	CHECK(handle(&s, "{\"metadata\":{},\"payload\":{}}", &e) == FF_ES_BAD); /* no type */
	CHECK(handle(&s, "{\"metadata\":{\"message_type\":\"session_welcome\"},\"payload\":{}}", &e) ==
	      FF_ES_BAD); /* no session */
	CHECK(handle(&s,
		     "{\"metadata\":{\"message_type\":\"notification\"},"
		     "\"payload\":{\"subscription\":{\"type\":\"channel.raid\"}}}",
		     &e) == FF_ES_BAD); /* no event */
	CHECK(handle(&s, NOTIF("channel.ban", "{\"user_name\":\"x\"}"), &e) == FF_ES_IGNORED);
	CHECK(strstr(s.note, "no handler") != NULL);
	CHECK(ff_es_handle(&s, NULL, &e) == FF_ES_BAD);

	/* ---- the subscription list ---- */
	CHECK(FF_ES_SUB_COUNT == 7);
	/* one subscription per alert kind, or a kind exists that nothing can ever fire */
	CHECK((int)FF_ES_SUB_COUNT == (int)FF_ALERT_KIND_COUNT);
	bool follow_v2 = false, raid_no_scope = false;
	for (size_t i = 0; i < FF_ES_SUB_COUNT; i++) {
		CHECK(FF_ES_SUBS[i].type && FF_ES_SUBS[i].type[0]);
		CHECK(FF_ES_SUBS[i].version && FF_ES_SUBS[i].version[0]);
		if (!strcmp(FF_ES_SUBS[i].type, "channel.follow"))
			follow_v2 = !strcmp(FF_ES_SUBS[i].version, "2") && FF_ES_SUBS[i].needs_moderator;
		if (!strcmp(FF_ES_SUBS[i].type, "channel.raid"))
			raid_no_scope = FF_ES_SUBS[i].scope[0] == 0;
	}
	/* follow is version 2 and needs moderator_user_id in its condition; version 1 is removed,
	   and a condition without the moderator id is refused at subscribe time */
	CHECK(follow_v2);
	CHECK(raid_no_scope);

	/* every type this handler answers is one we actually subscribe to */
	for (size_t i = 0; i < FF_ES_SUB_COUNT; i++) {
		char json[512];
		snprintf(json, sizeof json,
			 "{\"metadata\":{\"message_type\":\"notification\"},"
			 "\"payload\":{\"subscription\":{\"type\":\"%s\"},"
			 "\"event\":{\"user_name\":\"A\",\"from_broadcaster_user_name\":\"A\","
			 "\"tier\":\"1000\",\"is_gift\":false,\"bits\":1,\"viewers\":1}}}",
			 FF_ES_SUBS[i].type);
		enum ff_es_result r = handle(&s, json, &e);
		CHECK(r == FF_ES_EVENT);
		if (r != FF_ES_EVENT)
			fprintf(stderr, "      (no handler for %s)\n", FF_ES_SUBS[i].type);
	}

	CHECK(ff_es_tier("1000") == 1 && ff_es_tier("2000") == 2 && ff_es_tier("3000") == 3);
	CHECK(ff_es_tier("Prime") == 0 && ff_es_tier("") == 0 && ff_es_tier(NULL) == 0);

	FF_TEST_MAIN_END();
}
