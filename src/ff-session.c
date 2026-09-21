#include "ff-session.h"
#include "ff-twitch.h"

#include <stdio.h>
#include <string.h>

void ff_session_init(struct ff_session *s, const char *url)
{
	memset(s, 0, sizeof *s);
	snprintf(s->url, sizeof s->url, "%s", url ? url : "");
}

void ff_session_opened(struct ff_session *s, time_t now)
{
	/* The EventSub state is per SOCKET -- a new session sends its own welcome with its own id
	   and keepalive interval. `subscribed` is deliberately not touched: it belongs to the
	   subscriptions, which Twitch moves across for us. */
	memset(&s->es, 0, sizeof s->es);
	s->opened = now;
	s->last_message = now;
	s->reason[0] = 0;
}

enum ff_session_step ff_session_idle(struct ff_session *s, time_t now)
{
	if (!s->es.keepalive_secs) {
		/* No welcome yet, so there is no agreed keepalive and ff_twitch_is_stale refuses to
		   invent one. Without this a socket that completes the handshake and then says
		   nothing sits on "Connecting" forever, with no error and no retry. */
		if (now - s->opened > FF_SESSION_WELCOME_SECS) {
			snprintf(s->reason, sizeof s->reason,
				 "Twitch accepted the connection but never started the session");
			return FF_STEP_DROP;
		}
		return FF_STEP_NOTHING;
	}
	if (ff_twitch_is_stale(s->last_message, s->es.keepalive_secs, now)) {
		snprintf(s->reason, sizeof s->reason,
			 "Twitch went quiet for more than %d seconds", s->es.keepalive_secs * 2 + 2);
		return FF_STEP_DROP;
	}
	return FF_STEP_NOTHING;
}

enum ff_session_step ff_session_message(struct ff_session *s, const char *json, time_t now,
					struct ff_alert_event *out)
{
	s->last_message = now;
	s->reason[0] = 0;

	enum ff_es_result r = ff_es_handle(&s->es, json, out);
	switch (r) {
	case FF_ES_WELCOME:
		if (s->subscribed) {
			/* The welcome on a RECONNECT session. The subscriptions came with it, so
			   asking again returns 409 Conflict for every one -- and reading those as
			   refusals is what used to end the stream's alerts on a routine reconnect. */
			snprintf(s->reason, sizeof s->reason,
				 "reconnected; the subscriptions came across with the session");
			return FF_STEP_NOTE;
		}
		return FF_STEP_SUBSCRIBE;
	case FF_ES_KEEPALIVE:
		return FF_STEP_NOTHING;
	case FF_ES_EVENT:
		return FF_STEP_EMIT;
	case FF_ES_RECONNECT:
		snprintf(s->url, sizeof s->url, "%s", s->es.reconnect_url);
		return FF_STEP_RECONNECT;
	case FF_ES_REVOKED:
		/* ONE subscription stopped, because one scope was withdrawn. It is not the end of
		   the connection, and treating it as one stopped all seven alert types for a fault
		   in one. */
		snprintf(s->reason, sizeof s->reason, "%s", s->es.note);
		return FF_STEP_NOTE;
	case FF_ES_BAD:
		/* A message we cannot read is not a reason to drop a working connection -- Twitch
		   may add a field, or a message type, at any time. Logged and stepped over. */
		snprintf(s->reason, sizeof s->reason, "%s", s->es.err);
		return FF_STEP_NOTE;
	case FF_ES_IGNORED:
	default:
		if (s->es.note[0]) {
			snprintf(s->reason, sizeof s->reason, "%s", s->es.note);
			return FF_STEP_NOTE;
		}
		return FF_STEP_NOTHING;
	}
}

enum ff_session_step ff_session_subscribed(struct ff_session *s, int accepted, int total)
{
	/* Set whatever happened: a retry that re-subscribed from scratch would duplicate whatever
	   DID get through. */
	s->subscribed = true;
	if (accepted <= 0) {
		snprintf(s->reason, sizeof s->reason,
			 "Twitch accepted none of the %d alert types", total);
		return FF_STEP_DROP;
	}
	if (accepted < total) {
		/* Some work and some do not -- one missing scope takes out one alert type and the
		   rest are fine. Worth saying, not worth dropping. */
		snprintf(s->reason, sizeof s->reason, "only %d of %d alert types subscribed",
			 accepted, total);
		return FF_STEP_NOTE;
	}
	return FF_STEP_NOTHING;
}
