#include "ff-eventsub.h"

#include <obs-module.h>
#include <stdio.h>
#include <string.h>

/* Verified against dev.twitch.tv/docs/eventsub/eventsub-subscription-types (fetched 2026-09-21),
   not from memory: channel.subscription.gift and channel.subscription.message are their OWN
   types. foxfire/research/twitch-eventsub.md said gifts arrive inside channel.subscribe and
   resubs only through channel.chat.notification; both were wrong and the file has been corrected.

   channel.raid needs no scope, which is worth keeping in mind for a first-run experience: raids
   work before the streamer has granted anything else. */
const struct ff_es_sub FF_ES_SUBS[] = {
	{"channel.follow", "2", "moderator:read:followers", true},
	{"channel.subscribe", "1", "channel:read:subscriptions", false},
	{"channel.subscription.gift", "1", "channel:read:subscriptions", false},
	{"channel.subscription.message", "1", "channel:read:subscriptions", false},
	{"channel.cheer", "1", "bits:read", false},
	{"channel.raid", "1", "", false},
	{"channel.channel_points_custom_reward_redemption.add", "1", "channel:read:redemptions", false},
};
const size_t FF_ES_SUB_COUNT = sizeof FF_ES_SUBS / sizeof FF_ES_SUBS[0];

int ff_es_tier(const char *tier)
{
	if (!tier)
		return 0;
	if (!strcmp(tier, "1000"))
		return 1;
	if (!strcmp(tier, "2000"))
		return 2;
	if (!strcmp(tier, "3000"))
		return 3;
	return 0; /* "Prime", and anything Twitch adds later */
}

static void copy_into(char *dst, size_t cap, const char *src)
{
	snprintf(dst, cap, "%s", src ? src : "");
}

/* The display name, with the anonymous case handled where it happens rather than in the template.
   A gifter or cheerer may be anonymous, in which case Twitch sends the name as null or empty and
   obs_data_get_string returns "" -- an alert reading "  just gifted 5 subs" is the result of
   trusting that. */
static void name_into(char *dst, size_t cap, obs_data_t *ev, const char *field, bool anonymous)
{
	const char *n = obs_data_get_string(ev, field);
	if (anonymous || !n || !*n)
		n = obs_module_text("Foxfire.Alert.Anonymous");
	copy_into(dst, cap, n);
}

/* `message` is a plain string on channel.cheer and an OBJECT with a "text" member on
   channel.subscription.message. Reading it one way gives an empty resub message and no error. */
static void message_into(char *dst, size_t cap, obs_data_t *ev)
{
	const char *flat = obs_data_get_string(ev, "message");
	if (flat && *flat) {
		copy_into(dst, cap, flat);
		return;
	}
	obs_data_t *obj = obs_data_get_obj(ev, "message");
	if (obj) {
		copy_into(dst, cap, obs_data_get_string(obj, "text"));
		obs_data_release(obj);
		return;
	}
	dst[0] = 0;
}

static enum ff_es_result from_event(const char *type, obs_data_t *ev, struct ff_alert_event *out,
				    char *note, size_t notecap)
{
	memset(out, 0, sizeof *out);

	if (!strcmp(type, "channel.follow")) {
		out->kind = FF_ALERT_FOLLOW;
		name_into(out->name, sizeof out->name, ev, "user_name", false);
		return FF_ES_EVENT;
	}
	if (!strcmp(type, "channel.subscribe")) {
		/* A GIFTED sub fires this too, once per recipient. Twitch also sends the gifter a
		   channel.subscription.gift for the same action, so counting both turns one gift of
		   fifty subs into fifty-one alerts and buries the raid behind it. The gift event is
		   the one that names a human, so this side is dropped. */
		if (obs_data_get_bool(ev, "is_gift")) {
			snprintf(note, notecap, "a gifted sub, counted on the gift event instead");
			return FF_ES_IGNORED;
		}
		out->kind = FF_ALERT_SUB;
		out->tier = ff_es_tier(obs_data_get_string(ev, "tier"));
		name_into(out->name, sizeof out->name, ev, "user_name", false);
		return FF_ES_EVENT;
	}
	if (!strcmp(type, "channel.subscription.gift")) {
		out->kind = FF_ALERT_GIFT;
		out->tier = ff_es_tier(obs_data_get_string(ev, "tier"));
		/* `total` is how many were gifted in this action. A community gift of one still
		   sends 1, so an absent field meaning 0 would read as "gifted 0 subs". */
		out->amount = obs_data_get_int(ev, "total");
		if (out->amount < 1)
			out->amount = 1;
		name_into(out->name, sizeof out->name, ev, "user_name",
			  obs_data_get_bool(ev, "is_anonymous"));
		return FF_ES_EVENT;
	}
	if (!strcmp(type, "channel.subscription.message")) {
		out->kind = FF_ALERT_RESUB;
		out->tier = ff_es_tier(obs_data_get_string(ev, "tier"));
		out->amount = obs_data_get_int(ev, "cumulative_months");
		name_into(out->name, sizeof out->name, ev, "user_name", false);
		message_into(out->message, sizeof out->message, ev);
		return FF_ES_EVENT;
	}
	if (!strcmp(type, "channel.cheer")) {
		out->kind = FF_ALERT_BITS;
		out->amount = obs_data_get_int(ev, "bits");
		name_into(out->name, sizeof out->name, ev, "user_name",
			  obs_data_get_bool(ev, "is_anonymous"));
		message_into(out->message, sizeof out->message, ev);
		return FF_ES_EVENT;
	}
	if (!strcmp(type, "channel.raid")) {
		out->kind = FF_ALERT_RAID;
		out->amount = obs_data_get_int(ev, "viewers");
		/* the raider is from_broadcaster_user_name; user_name is not in this payload at
		   all, and reading it would name every raid "" */
		name_into(out->name, sizeof out->name, ev, "from_broadcaster_user_name", false);
		return FF_ES_EVENT;
	}
	if (!strcmp(type, "channel.channel_points_custom_reward_redemption.add")) {
		out->kind = FF_ALERT_REDEEM;
		name_into(out->name, sizeof out->name, ev, "user_name", false);
		/* what they typed, if the reward asks for input */
		copy_into(out->message, sizeof out->message, obs_data_get_string(ev, "user_input"));
		obs_data_t *reward = obs_data_get_obj(ev, "reward");
		if (reward) {
			const char *title = obs_data_get_string(reward, "title");
			if (title && *title && !out->message[0])
				copy_into(out->message, sizeof out->message, title);
			obs_data_release(reward);
		}
		return FF_ES_EVENT;
	}

	/* A type we never subscribed to. Twitch does not send those, so this means our own list
	   and our own handler disagree -- said out loud rather than dropped. */
	snprintf(note, notecap, "no handler for subscription type '%s'", type);
	return FF_ES_IGNORED;
}

enum ff_es_result ff_es_handle(struct ff_es *s, const char *json, struct ff_alert_event *out)
{
	if (!s || !json || !out) {
		if (s)
			snprintf(s->err, sizeof s->err, "nothing to handle");
		return FF_ES_BAD;
	}
	s->note[0] = 0;
	s->err[0] = 0;

	obs_data_t *root = obs_data_create_from_json(json);
	if (!root) {
		snprintf(s->err, sizeof s->err, "the message is not JSON");
		return FF_ES_BAD;
	}
	obs_data_t *meta = obs_data_get_obj(root, "metadata");
	if (!meta) {
		snprintf(s->err, sizeof s->err, "the message has no metadata");
		obs_data_release(root);
		return FF_ES_BAD;
	}
	const char *type = obs_data_get_string(meta, "message_type");
	enum ff_es_result r = FF_ES_IGNORED;
	obs_data_t *payload = obs_data_get_obj(root, "payload");

	if (!type || !*type) {
		snprintf(s->err, sizeof s->err, "the message has no message_type");
		r = FF_ES_BAD;
	} else if (!strcmp(type, "session_welcome") || !strcmp(type, "session_reconnect")) {
		obs_data_t *sess = payload ? obs_data_get_obj(payload, "session") : NULL;
		if (!sess) {
			snprintf(s->err, sizeof s->err, "a %s with no session", type);
			r = FF_ES_BAD;
		} else {
			copy_into(s->session_id, sizeof s->session_id,
				  obs_data_get_string(sess, "id"));
			/* Twitch picks this, between 10 and 600 seconds, and silence for longer
			   than it means the connection is gone. Defaulting to something when it is
			   missing would invent a timeout the server never agreed to. */
			s->keepalive_secs = (int)obs_data_get_int(sess, "keepalive_timeout_seconds");
			const char *ru = obs_data_get_string(sess, "reconnect_url");
			copy_into(s->reconnect_url, sizeof s->reconnect_url, ru);
			if (!strcmp(type, "session_reconnect")) {
				if (!s->reconnect_url[0]) {
					snprintf(s->err, sizeof s->err,
						 "a reconnect with no URL to reconnect to");
					r = FF_ES_BAD;
				} else {
					r = FF_ES_RECONNECT;
				}
			} else if (!s->session_id[0]) {
				snprintf(s->err, sizeof s->err, "a welcome with no session id");
				r = FF_ES_BAD;
			} else {
				r = FF_ES_WELCOME;
			}
			obs_data_release(sess);
		}
	} else if (!strcmp(type, "session_keepalive")) {
		r = FF_ES_KEEPALIVE;
	} else if (!strcmp(type, "notification")) {
		obs_data_t *sub = payload ? obs_data_get_obj(payload, "subscription") : NULL;
		obs_data_t *ev = payload ? obs_data_get_obj(payload, "event") : NULL;
		const char *st = sub ? obs_data_get_string(sub, "type") : NULL;
		if (!st || !*st || !ev) {
			snprintf(s->err, sizeof s->err, "a notification with no %s",
				 (!st || !*st) ? "subscription type" : "event");
			r = FF_ES_BAD;
		} else {
			r = from_event(st, ev, out, s->note, sizeof s->note);
		}
		obs_data_release(sub);
		obs_data_release(ev);
	} else if (!strcmp(type, "revocation")) {
		obs_data_t *sub = payload ? obs_data_get_obj(payload, "subscription") : NULL;
		/* A revocation is the streamer's alerts quietly stopping: the token was refused,
		   or the scope was removed. It has to reach the panel, not just a log line. */
		snprintf(s->note, sizeof s->note, "Twitch stopped '%s': %s",
			 sub ? obs_data_get_string(sub, "type") : "a subscription",
			 sub ? obs_data_get_string(sub, "status") : "no reason given");
		obs_data_release(sub);
		r = FF_ES_REVOKED;
	} else {
		snprintf(s->note, sizeof s->note, "a '%s' message, which we do not act on", type);
		r = FF_ES_IGNORED;
	}

	obs_data_release(payload);
	obs_data_release(meta);
	obs_data_release(root);
	return r;
}
