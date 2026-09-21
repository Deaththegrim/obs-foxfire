#pragma once
#include "ff-alert-queue.h"
#include "ff-twitch-api.h"

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* The connection, as a thing that runs by itself.
 *
 * One worker thread owns every network object; the UI thread only sets flags and reads a status
 * string. Nothing else may touch the socket, which is the whole reason this is a thread and not a
 * set of calls: an OBS properties callback runs on the UI thread, and a five-second HTTP request
 * there freezes the whole program.
 *
 * The two decisions that actually matter when this goes wrong are pure functions below, because
 * "alerts quietly stopped an hour into the stream" is the failure, and it is not one you can
 * reproduce by clicking.
 */

enum ff_twitch_state {
	FF_TWS_OFF = 0,     /* not signed in, or switched off */
	FF_TWS_SIGNING_IN,  /* the streamer has a code to type */
	FF_TWS_CONNECTING,
	FF_TWS_LIVE,
	FF_TWS_RETRYING,    /* lost it, waiting to try again */
	FF_TWS_FAILED,      /* needs the streamer: sign in again, or grant a scope */
};

/* How long to wait before retry number `attempt` (1-based).
 *
 * Twitch's own guidance is to back off, and a client that reconnects instantly after a drop is
 * indistinguishable from an attack. Capped, because a stream that recovers after ten minutes of
 * no alerts has already lost the moment -- an unbounded doubling would reach hours. */
int ff_twitch_backoff_secs(int attempt);

/* Has the connection gone quiet for longer than it should have?
 *
 * EventSub sends a keepalive every `keepalive_secs` when nothing else is happening, so silence
 * past that is the ONLY way to notice a connection that is open at the socket level and dead at
 * the service level -- which is what a dropped wifi link looks like from in here. The allowance
 * is for one missed keepalive, not zero, or a busy machine reconnects for nothing. */
bool ff_twitch_is_stale(time_t last_message, int keepalive_secs, time_t now);

struct ff_twitch;

/* Called from the WORKER thread with each event. It must be cheap and thread-safe; pushing onto
   the alert queue is both. */
typedef void (*ff_twitch_event_cb)(void *ctx, const struct ff_alert_event *e);

struct ff_twitch *ff_twitch_create(ff_twitch_event_cb cb, void *ctx, const char *module_path);
void ff_twitch_destroy(struct ff_twitch *t);

/* The Twitch application's client id. There is no default and no fallback: a wrong or missing
   one has to be visible, not guessed at. */
void ff_twitch_set_client_id(struct ff_twitch *t, const char *client_id);

void ff_twitch_set_enabled(struct ff_twitch *t, bool on);
void ff_twitch_sign_in(struct ff_twitch *t);  /* starts the device flow */
void ff_twitch_sign_out(struct ff_twitch *t); /* forgets the saved token */

/* A line for the properties panel, and the code to type while signing in. Both safe from the UI
   thread. `code`/`url` are empty unless the state is SIGNING_IN. */
enum ff_twitch_state ff_twitch_status(struct ff_twitch *t, char *line, size_t linecap, char *code,
				      size_t codecap, char *url, size_t urlcap);
