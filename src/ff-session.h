#pragma once
#include "ff-alert-queue.h"
#include "ff-eventsub.h"

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* One EventSub session's DECISIONS, with no I/O in them at all.
 *
 * This exists because of what went wrong when it did not. Four of the defects a review found in
 * this plugin lived in the loop that used to make these decisions inline, and one of them --
 * re-subscribing after a session_reconnect, which Twitch answers 409 to, which the code read as
 * "refused every alert type" and then gave up on permanently -- fires on a ROUTINE reconnect.
 * It survived to review because nothing could drive that loop without a Twitch account, a live
 * channel, and Twitch choosing to redeploy.
 *
 * Everything here takes `now` as an argument and returns what the caller should do. A test drives
 * it with scripted messages and a clock it invents. src/ff-twitch.c is then only plumbing.
 */

enum ff_session_step {
	FF_STEP_NOTHING = 0, /* carry on */
	FF_STEP_SUBSCRIBE,   /* welcomed: create the subscriptions for `session_id` */
	FF_STEP_EMIT,        /* `out` holds an alert to show */
	FF_STEP_RECONNECT,   /* open `url` instead; the subscriptions come with us */
	FF_STEP_DROP,        /* end this connection and retry; `reason` says why */
	FF_STEP_NOTE,        /* worth logging, nothing to do; `reason` says what */
};

/* How long to wait for a session_welcome before giving up on a socket that opened and then said
   nothing. It cannot come from Twitch, because the value that would tell us arrives IN the
   welcome -- so a connection that never gets one has no timeout at all unless it is this one. */
#define FF_SESSION_WELCOME_SECS 30

/* Twitch gives thirty seconds between sending a session_reconnect and closing the old socket --
   specifically so a client does not have to drop anything. Beyond that the old one is going away
   whether we are ready or not, so the handover is abandoned and an ordinary reconnect happens. */
#define FF_SESSION_HANDOVER_SECS 30

struct ff_session {
	struct ff_es es;
	/* Survives a reconnect ON PURPOSE: EventSub moves the subscriptions to the new session,
	   and asking again duplicates every alert at best and 409s at worst. */
	bool subscribed;
	time_t opened;       /* when the CURRENT socket came up */
	/* Non-zero while a replacement socket is being brought up alongside this one. The old
	   connection keeps delivering until the new one is welcomed, which is the whole point:
	   Twitch keeps sending on the old socket during the changeover, and tearing it down first
	   loses whatever arrives in the gap. A raid landing there is the moment that matters. */
	time_t handover_started;
	time_t last_message;
	char url[1024];
	char reason[512];
};

void ff_session_init(struct ff_session *s, const char *url);

/* A socket is up. Resets the per-connection clocks and NOT `subscribed`. */
void ff_session_opened(struct ff_session *s, time_t now);

enum ff_session_step ff_session_message(struct ff_session *s, const char *json, time_t now,
					struct ff_alert_event *out);

/* Nothing arrived. This is where a connection that is open at the socket and dead at the service
   gets noticed -- the only way to see it. */
enum ff_session_step ff_session_idle(struct ff_session *s, time_t now);

/* The result of acting on FF_STEP_SUBSCRIBE. `accepted` of `total` types were taken. */
enum ff_session_step ff_session_subscribed(struct ff_session *s, int accepted, int total);

/* ---- the changeover ----
 *
 * On FF_STEP_RECONNECT the caller opens a SECOND socket at `url` and runs a second ff_session for
 * it, while continuing to poll the first. These say when to swap and when to give up. */

bool ff_session_in_handover(const struct ff_session *s);

/* Has this one been welcomed? True once a session_welcome has given it an id -- which is the
   signal that Twitch has moved to it and the old socket can go. */
bool ff_session_welcomed(const struct ff_session *s);

/* The thirty seconds are up and the replacement never welcomed. */
bool ff_session_handover_expired(const struct ff_session *s, time_t now);

/* Take the replacement's session over, keeping `subscribed` (the subscriptions moved with it). */
void ff_session_adopt(struct ff_session *s, const struct ff_session *pending, time_t now);

/* Prepares a session for a socket that is replacing an existing one: it is already subscribed,
   so its welcome must NOT trigger a second subscribe. */
void ff_session_init_replacement(struct ff_session *s, const char *url);
