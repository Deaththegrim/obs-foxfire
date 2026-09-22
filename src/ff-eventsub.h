#pragma once
#include "ff-alert-queue.h"

#include <stdbool.h>
#include <stddef.h>

/* Turning EventSub's WebSocket messages into alerts.
 *
 * Pure: a JSON string in, a decision out. No socket, no thread, no OBS. Every message shape Twitch
 * can send is a string that can be written down, so all of this is testable without an account, a
 * token, or a network -- which matters, because the alternative is finding out that gift subs are
 * mis-attributed during someone's actual stream.
 */

enum ff_es_result {
	FF_ES_IGNORED = 0, /* a message type, or an event, we deliberately do nothing with */
	FF_ES_WELCOME,     /* session_id is set; the caller must now create its subscriptions */
	FF_ES_KEEPALIVE,   /* proof of life; reset the timeout */
	FF_ES_EVENT,       /* `out` is filled */
	FF_ES_RECONNECT,   /* reconnect_url is set, and there are 30 seconds to use it */
	FF_ES_REVOKED,     /* a subscription stopped; `note` says why */
	FF_ES_BAD,         /* not a message we can read at all; `err` says what was wrong */
};

struct ff_es {
	char session_id[128];
	char reconnect_url[1024];
	int keepalive_secs;
	char note[256];
	char err[256];
};

/* Handles one message. `out` is only written for FF_ES_EVENT. */
enum ff_es_result ff_es_handle(struct ff_es *s, const char *json, struct ff_alert_event *out);

/* The subscription types this plugin asks for, in the order it asks. Exposed so the subscribe
   step and the tests read from the same list rather than two lists that drift apart. */
struct ff_es_sub {
	const char *type;
	const char *version;
	const char *scope;    /* the OAuth scope it needs; "" when none */
	bool needs_moderator; /* condition carries moderator_user_id as well as broadcaster */
};
extern const struct ff_es_sub FF_ES_SUBS[];
extern const size_t FF_ES_SUB_COUNT;

/* "1000" -> 1, "2000" -> 2, "3000" -> 3, "Prime"/anything else -> 0.
 *
 * Twitch sends tier as a STRING. obs_data_get_int() on a string key returns 0 with no error, so
 * reading it as a number would silently make every subscriber Prime. */
int ff_es_tier(const char *tier);
