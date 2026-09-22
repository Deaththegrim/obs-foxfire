#include "ff-twitch.h"
#include "ff-net.h"
#include "ff-session.h"

#include <obs-module.h>
#include <plugin-support.h>
#include "ff-compat.h"
#include <pthread.h>
#include <stdarg.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <stdlib.h>
#include <string.h>

#define FF_TW_URL "wss://eventsub.wss.twitch.tv/ws"

int ff_twitch_backoff_secs(int attempt)
{
	/* 1, 2, 4, 8, 16, 32, 60, 60, ...
	   No floor on `attempt` is needed: the loop below does not run for anything <= 1, so 0 and
	   negatives already return 1. There WAS one, and removing it changed no result in 97
	   checks -- an equivalent mutant, which means the guard was dead code rather than the test
	   being thin. */
	int d = 1;
	for (int i = 1; i < attempt && d < 60; i++)
		d *= 2;
	return d > 60 ? 60 : d;
}

bool ff_twitch_is_stale(time_t last_message, int keepalive_secs, time_t now)
{
	/* Twitch picks the keepalive between 10 and 600 seconds and sends it in the welcome. A
	   value we never received is not something to invent a timeout from -- 0 here would make
	   every connection instantly stale and reconnect forever. */
	if (keepalive_secs <= 0)
		return false;
	if (last_message <= 0)
		return false;
	/* one missed keepalive is allowed, plus a couple of seconds of slack for a machine that
	   was busy encoding */
	return now > last_message + (time_t)(keepalive_secs * 2 + 2);
}

struct ff_twitch {
	pthread_t thread;
	pthread_mutex_t lock;
	bool running;   /* the thread should keep going */
	bool enabled;   /* the streamer wants a connection */
	bool want_sign_in;
	bool want_sign_out;

	char client_id[256];
	char module_path[512];

	enum ff_twitch_state state;
	char line[512];
	char code[32];
	char verify_url[256];

	ff_twitch_event_cb cb;
	ff_twitch_changed_cb on_change;
	void *ctx;
};

static const char *state_name(enum ff_twitch_state s)
{
	switch (s) {
	case FF_TWS_OFF:
		return "off";
	case FF_TWS_SIGNING_IN:
		return "signing in";
	case FF_TWS_CONNECTING:
		return "connecting";
	case FF_TWS_LIVE:
		return "live";
	case FF_TWS_RETRYING:
		return "retrying";
	case FF_TWS_FAILED:
		return "failed";
	}
	return "?";
}

static void set_line(struct ff_twitch *t, enum ff_twitch_state s, const char *line)
{
	pthread_mutex_lock(&t->lock);
	bool changed = t->state != s || strcmp(t->line, line) != 0;
	t->state = s;
	snprintf(t->line, sizeof t->line, "%s", line);
	if (s != FF_TWS_SIGNING_IN) {
		t->code[0] = 0;
		t->verify_url[0] = 0;
	}
	ff_twitch_changed_cb notify = t->on_change;
	void *ctx = t->ctx;
	pthread_mutex_unlock(&t->lock);

	if (!changed)
		return;
	/* Logged, every transition, because the properties dialog is CLOSED during a stream and
	   this is the only record that survives. "Alerts stopped an hour in" used to leave nothing
	   in the OBS log at all -- the reason strings were written, and then thrown away. */
	obs_log(s == FF_TWS_FAILED || s == FF_TWS_RETRYING ? LOG_WARNING : LOG_INFO,
		"twitch: %s -- %s", state_name(s), line);
	/* and pushed, so a panel that happens to be open updates itself rather than showing
	   whatever was true when it was opened */
	if (notify)
		notify(ctx);
}

/* A printf format, and the compiler is told so.
 *
 * The attribute is the point of this function existing separately. Without it, gcc cannot check
 * a format it cannot see, and the version of this file that shipped passed OBS locale strings
 * straight to vsnprintf -- where "%1" is a VALID conversion (width 1, then whatever follows).
 * "Reconnecting in %1s" with an int argument is width-1 "%s": it dereferences the integer as a
 * pointer and segfaults, on the ordinary path taken every time a connection drops. Measured, not
 * theorised: the exact call crashed with exit 139.
 *
 * So: this one takes literal formats with C arguments, ff_localise() below takes OBS strings with
 * %1/%2/%3, and the two never meet. */
FF_PRINTF(3, 4) static void set_state(struct ff_twitch *t, enum ff_twitch_state s, const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	set_line(t, s, line);
}

/* An OBS locale string, whose placeholders are %1/%2/%3 and are substituted by text replacement,
   which is what the rest of this plugin does (see ff-layers.c). NULL arguments are skipped, so a
   string using only %1 is fine. */
void ff_twitch_format(const char *key, const char *a1, const char *a2, const char *a3, char *out,
		      size_t cap)
{
	struct dstr text = {0};
	dstr_copy(&text, obs_module_text(key));
	/* Highest placeholder first. Replacing %1 before %2 means a VALUE containing "%2" would
	   then be substituted again -- a username is hostile input, and "%2" in one is a perfectly
	   ordinary thing for someone to type. */
	if (a3)
		dstr_replace(&text, "%3", a3);
	if (a2)
		dstr_replace(&text, "%2", a2);
	if (a1)
		dstr_replace(&text, "%1", a1);
	snprintf(out, cap, "%s", text.array ? text.array : "");
	dstr_free(&text);
}

static void set_text(struct ff_twitch *t, enum ff_twitch_state st, const char *key, const char *a1,
		     const char *a2, const char *a3)
{
	char line[512];
	ff_twitch_format(key, a1, a2, a3, line, sizeof line);
	set_line(t, st, line);
}

enum ff_twitch_state ff_twitch_status(struct ff_twitch *t, char *line, size_t linecap, char *code,
				      size_t codecap, char *url, size_t urlcap)
{
	if (!t)
		return FF_TWS_OFF;
	pthread_mutex_lock(&t->lock);
	enum ff_twitch_state s = t->state;
	if (line)
		snprintf(line, linecap, "%s", t->line);
	if (code)
		snprintf(code, codecap, "%s", t->code);
	if (url)
		snprintf(url, urlcap, "%s", t->verify_url);
	pthread_mutex_unlock(&t->lock);
	return s;
}

void ff_twitch_set_client_id(struct ff_twitch *t, const char *client_id)
{
	if (!t)
		return;
	pthread_mutex_lock(&t->lock);
	snprintf(t->client_id, sizeof t->client_id, "%s", client_id ? client_id : "");
	pthread_mutex_unlock(&t->lock);
}

void ff_twitch_set_enabled(struct ff_twitch *t, bool on)
{
	if (!t)
		return;
	pthread_mutex_lock(&t->lock);
	t->enabled = on;
	pthread_mutex_unlock(&t->lock);
}

void ff_twitch_sign_in(struct ff_twitch *t)
{
	if (!t)
		return;
	pthread_mutex_lock(&t->lock);
	t->want_sign_in = true;
	pthread_mutex_unlock(&t->lock);
}

void ff_twitch_sign_out(struct ff_twitch *t)
{
	if (!t)
		return;
	pthread_mutex_lock(&t->lock);
	t->want_sign_out = true;
	pthread_mutex_unlock(&t->lock);
}

static bool taking(struct ff_twitch *t, bool *flag)
{
	pthread_mutex_lock(&t->lock);
	bool v = *flag;
	*flag = false;
	pthread_mutex_unlock(&t->lock);
	return v;
}

static bool reading(struct ff_twitch *t, bool *flag)
{
	pthread_mutex_lock(&t->lock);
	bool v = *flag;
	pthread_mutex_unlock(&t->lock);
	return v;
}

static void copy_client_id(struct ff_twitch *t, char *out, size_t cap)
{
	pthread_mutex_lock(&t->lock);
	snprintf(out, cap, "%s", t->client_id);
	pthread_mutex_unlock(&t->lock);
}

/* Sleeps in slices so that switching the source off, or closing OBS, does not wait out a
   sixty-second backoff. A shutdown that takes a minute reads as a hang and gets killed. */
static void nap(struct ff_twitch *t, int secs)
{
	for (int i = 0; i < secs * 5 && reading(t, &t->running); i++)
		os_sleep_ms(200);
}

/* The device flow, on the worker thread. Polls until the streamer finishes, gives up, or the code
   expires. Returns true when a refresh token was saved. */
static bool do_sign_in(struct ff_twitch *t)
{
	char cid[256];
	copy_client_id(t, cid, sizeof cid);
	if (!cid[0]) {
		set_state(t, FF_TWS_FAILED, "%s", obs_module_text("Foxfire.Twitch.NoClientId"));
		return false;
	}
	char err[512] = {0};
	struct ff_twitch_device dev;
	if (!ff_twitch_device_start(cid, &dev, err, sizeof err)) {
		set_state(t, FF_TWS_FAILED, "%s", err);
		return false;
	}
	pthread_mutex_lock(&t->lock);
	snprintf(t->code, sizeof t->code, "%s", dev.user_code);
	snprintf(t->verify_url, sizeof t->verify_url, "%s", dev.verify_url);
	pthread_mutex_unlock(&t->lock);
	set_text(t, FF_TWS_SIGNING_IN, "Foxfire.Twitch.EnterCode", dev.user_code, dev.verify_url,
		 NULL);

	int interval = dev.interval;
	time_t deadline = time(NULL) + (dev.expires_in > 0 ? dev.expires_in : 1800);
	while (reading(t, &t->running) && time(NULL) < deadline) {
		nap(t, interval);
		if (!reading(t, &t->running))
			return false;
		struct ff_twitch_token tok;
		enum ff_twitch_poll r = ff_twitch_device_poll(cid, dev.device_code, &tok, err,
							      sizeof err);
		if (r == FF_TW_GOT_TOKEN) {
			if (!ff_twitch_token_save(t->module_path, &tok)) {
				/* A sign-in we cannot store is not a sign-in. Reporting success here
				   sent the streamer through the whole device flow and then told them
				   they were not signed in, with one warning buried in the log. */
				set_text(t, FF_TWS_FAILED, "Foxfire.Twitch.CannotStore", NULL, NULL,
					 NULL);
				return false;
			}
			set_state(t, FF_TWS_CONNECTING, "%s",
				  obs_module_text("Foxfire.Twitch.SignedIn"));
			return true;
		}
		if (r == FF_TW_SLOW_DOWN) {
			interval += 5; /* asked to slow down: obey, or be rate limited harder */
			continue;
		}
		if (r == FF_TW_PENDING)
			continue;
		set_state(t, FF_TWS_FAILED, "%s", err);
		return false;
	}
	if (reading(t, &t->running))
		set_state(t, FF_TWS_FAILED, "%s", obs_module_text("Foxfire.Twitch.CodeExpired"));
	return false;
}

/* One whole connection: refresh, identify, open, subscribe, then read until it ends. Returns true
   if it ran long enough to count as a success, which is what resets the backoff -- a connection
   that dies after ten minutes is not the same failure as one that dies during the handshake, and
   treating them the same either hammers Twitch or gives up on a flaky link. */
static bool run_once(struct ff_twitch *t)
{
	char cid[256];
	copy_client_id(t, cid, sizeof cid);
	char err[512] = {0};

	struct ff_twitch_token saved;
	if (!ff_twitch_token_load(t->module_path, &saved)) {
		set_state(t, FF_TWS_OFF, "%s", obs_module_text("Foxfire.Twitch.NotSignedIn"));
		return false;
	}
	struct ff_twitch_token tok;
	enum ff_refresh_result rr = ff_twitch_refresh(cid, saved.refresh, &tok, err, sizeof err);
	if (rr == FF_REFRESH_REJECTED) {
		/* Twitch ANSWERED and said no -- disconnected in its settings, or a password
		   change. Retrying cannot fix it, so the token goes and the panel says sign in
		   again rather than looping forever on RETRYING. */
		ff_twitch_token_forget(t->module_path);
		set_state(t, FF_TWS_FAILED, "%s", err);
		return false;
	}
	if (rr == FF_REFRESH_UNREACHABLE) {
		/* We could not reach Twitch. The token is probably fine, and deleting it here is
		   how a router reboot costs someone their sign-in mid-stream -- it used to. */
		set_state(t, FF_TWS_RETRYING, "%s", err);
		return false;
	}
	/* Twitch rotates the refresh token on use and INVALIDATES the old one, so failing to store
	   the new one leaves a dead token on disk. This session keeps working, and the next OBS
	   start fails -- with nothing to connect the two. Said out loud now. */
	if (tok.refresh[0] && !ff_twitch_token_save(t->module_path, &tok))
		obs_log(LOG_WARNING,
			"twitch: could not store the renewed sign-in; it will have to be done "
			"again next time OBS starts");

	char uid[64] = {0}, login[64] = {0};
	if (!ff_twitch_user_id(cid, tok.access, uid, sizeof uid, login, sizeof login, err,
			       sizeof err)) {
		set_state(t, FF_TWS_RETRYING, "%s", err);
		return false;
	}

	/* Every decision from here on is ff-session.c's, and nothing in this function makes one.
	   That split is the point: four of the defects a review found lived in the version of this
	   loop that decided inline, and the one that fired on every routine reconnect survived
	   because there was no way to drive it without a live Twitch session. */
	struct ff_session sess;
	ff_session_init(&sess, FF_TW_URL);
	bool ran_long = false;

	for (;;) { /* one pass per socket; a session_reconnect opens another at a new URL */
		set_text(t, FF_TWS_CONNECTING, "Foxfire.Twitch.Connecting", login, NULL, NULL);
		struct ff_net *net = ff_net_ws_open(sess.url, err, sizeof err);
		if (!net) {
			set_state(t, FF_TWS_RETRYING, "%s", err);
			return ran_long;
		}
		ff_session_opened(&sess, time(NULL));
		time_t started = time(NULL);
		bool reconnecting = false;
		/* The replacement socket, brought up alongside this one during a changeover. Twitch
		   keeps delivering on the OLD socket until the new one is welcomed, so both are
		   polled and the old one is not closed until the swap. */
		struct ff_net *pending = NULL;
		struct ff_session psess;

		while (reading(t, &t->running) && reading(t, &t->enabled)) {
			/* The replacement first, so the swap happens as soon as it is welcomed and
			   the old socket stops being read a moment later rather than a poll later. */
			if (pending) {
				struct ff_ws_msg pm;
				struct ff_alert_event pev;
				int pr = ff_ws_conn_poll(ff_net_conn(pending), &pm);
				if (pr < 0) {
					obs_log(LOG_WARNING,
						"twitch: the replacement connection failed (%s); "
						"staying on the current one",
						ff_net_conn(pending)->err);
					ff_net_close(pending);
					pending = NULL;
				} else if (pr == 1) {
					char *pj = malloc(pm.len + 1);
					if (pj) {
						memcpy(pj, pm.payload, pm.len);
						pj[pm.len] = 0;
						ff_session_message(&psess, pj, time(NULL), &pev);
						free(pj);
					}
					if (ff_session_welcomed(&psess)) {
						/* Twitch has moved. Now, and not before, the old
						   socket can go. */
						ff_ws_conn_close(ff_net_conn(net), 1000);
						ff_net_close(net);
						net = pending;
						pending = NULL;
						ff_session_adopt(&sess, &psess, time(NULL));
						started = time(NULL);
						obs_log(LOG_INFO,
							"twitch: moved to the replacement connection "
							"without dropping the old one");
						continue;
					}
				}
				if (pending && ff_session_handover_expired(&sess, time(NULL))) {
					/* The thirty seconds are up. The old socket is going away
					   whether we are ready or not, so stop straddling and
					   reconnect the ordinary way. */
					obs_log(LOG_WARNING,
						"twitch: the replacement connection never started a "
						"session; reconnecting");
					ff_net_close(pending);
					pending = NULL;
					reconnecting = true;
					break;
				}
			}

			struct ff_ws_msg m;
			int r = ff_ws_conn_poll(ff_net_conn(net), &m);
			if (r < 0) {
				set_state(t, FF_TWS_RETRYING, "%s", ff_net_conn(net)->err);
				break;
			}

			struct ff_alert_event ev;
			enum ff_session_step step;
			if (r == 0) {
				step = ff_session_idle(&sess, time(NULL));
				if (step == FF_STEP_NOTHING) {
					ff_net_wait(net, 200);
					continue;
				}
			} else {
				/* the payload points into the read buffer and is not terminated */
				char *json = malloc(m.len + 1);
				if (!json) {
					set_state(t, FF_TWS_RETRYING, "%s",
						  "ran out of memory reading an event from Twitch");
					break;
				}
				memcpy(json, m.payload, m.len);
				json[m.len] = 0;
				step = ff_session_message(&sess, json, time(NULL), &ev);
				free(json);
			}

			if (step == FF_STEP_SUBSCRIBE) {
				int ok = 0;
				for (size_t i = 0; i < FF_ES_SUB_COUNT; i++) {
					long status = 0;
					if (ff_twitch_subscribe(cid, tok.access, &FF_ES_SUBS[i], uid,
								sess.es.session_id, &status, err,
								sizeof err))
						ok++;
					else
						/* Named, so the reason the follows never arrive is
						   findable: one missing scope takes out one alert
						   type and the rest still work. */
						obs_log(LOG_WARNING, "twitch: %s", err);
				}
				step = ff_session_subscribed(&sess, ok, (int)FF_ES_SUB_COUNT);
				if (step != FF_STEP_DROP) {
					char n_ok[16], n_all[16];
					snprintf(n_ok, sizeof n_ok, "%d", ok);
					snprintf(n_all, sizeof n_all, "%d", (int)FF_ES_SUB_COUNT);
					set_text(t, FF_TWS_LIVE, "Foxfire.Twitch.Live", n_ok, n_all,
						 login);
				}
			}

			if (step == FF_STEP_EMIT) {
				if (t->cb)
					t->cb(t->ctx, &ev);
			} else if (step == FF_STEP_RECONNECT) {
				/* Open the replacement NEXT TO this one. Twitch gives thirty
				   seconds precisely so nothing has to be dropped, and this used to
				   close first and lose whatever arrived in the gap. */
				if (pending)
					ff_net_close(pending);
				pending = ff_net_ws_open(sess.url, err, sizeof err);
				if (!pending) {
					/* could not bring one up: fall back to closing and
					   reopening, which is worse but still works */
					obs_log(LOG_WARNING,
						"twitch: could not open the replacement connection "
						"(%s); reconnecting the slow way",
						err);
					reconnecting = true;
					break;
				}
				ff_session_init_replacement(&psess, sess.url);
				ff_session_opened(&psess, time(NULL));
			} else if (step == FF_STEP_DROP) {
				set_state(t, FF_TWS_RETRYING, "%s", sess.reason);
				break;
			} else if (step == FF_STEP_NOTE && sess.reason[0]) {
				obs_log(LOG_INFO, "twitch: %s", sess.reason);
			}

			if (time(NULL) - started > 60)
				ran_long = true;
		}

		if (pending)
			ff_net_close(pending);
		ff_ws_conn_close(ff_net_conn(net), 1000);
		ff_net_close(net);
		if (!reconnecting)
			return ran_long;
	}
}

static void *worker(void *arg)
{
	struct ff_twitch *t = arg;
	int attempt = 0;
	while (reading(t, &t->running)) {
		if (taking(t, &t->want_sign_out)) {
			ff_twitch_token_forget(t->module_path);
			set_state(t, FF_TWS_OFF, "%s", obs_module_text("Foxfire.Twitch.SignedOut"));
		}
		if (taking(t, &t->want_sign_in)) {
			if (do_sign_in(t))
				attempt = 0;
			continue;
		}
		if (!reading(t, &t->enabled)) {
			nap(t, 1);
			continue;
		}
		pthread_mutex_lock(&t->lock);
		enum ff_twitch_state now = t->state;
		pthread_mutex_unlock(&t->lock);
		if (now == FF_TWS_FAILED || now == FF_TWS_OFF) {
			/* Both mean a person has to do something -- FAILED needs a fix, OFF means
			   nobody has signed in yet. Retrying either would hide it behind a spinner.
			   OFF was missing here: run_once returns to OFF in microseconds when there
			   is no token, and the bottom of this loop skips the backoff for OFF too,
			   so ticking the enable box before signing in span a core flat out for as
			   long as OBS was open. Nothing logged it; the only symptom was the fan. */
			nap(t, 1);
			continue;
		}

		if (run_once(t))
			attempt = 0;
		else
			attempt++;

		if (!reading(t, &t->running) || !reading(t, &t->enabled))
			continue;
		pthread_mutex_lock(&t->lock);
		bool stop = t->state == FF_TWS_FAILED || t->state == FF_TWS_OFF;
		pthread_mutex_unlock(&t->lock);
		if (stop) {
			nap(t, 1); /* never loop without sleeping; see the note above */
			continue;
		}
		int wait = ff_twitch_backoff_secs(attempt);
		char n_wait[16];
		snprintf(n_wait, sizeof n_wait, "%d", wait);
		set_text(t, FF_TWS_RETRYING, "Foxfire.Twitch.Retrying", n_wait, NULL, NULL);
		nap(t, wait);
	}
	return NULL;
}

struct ff_twitch *ff_twitch_create(ff_twitch_event_cb cb, ff_twitch_changed_cb on_change,
				   void *ctx, const char *module_path)
{
	struct ff_twitch *t = bzalloc(sizeof *t);
	if (pthread_mutex_init(&t->lock, NULL) != 0) {
		obs_log(LOG_ERROR, "twitch: could not create the connection's lock");
		bfree(t);
		return NULL;
	}
	t->cb = cb;
	t->on_change = on_change;
	t->ctx = ctx;
	t->running = true;
	snprintf(t->module_path, sizeof t->module_path, "%s", module_path ? module_path : "");
	snprintf(t->line, sizeof t->line, "%s", obs_module_text("Foxfire.Twitch.NotSignedIn"));
	int rc = pthread_create(&t->thread, NULL, worker, t);
	if (rc != 0) {
		/* Said out loud. Silently returning NULL leaves both panel buttons inert and the
		   status line stuck on "Not signed in" forever -- the streamer presses Connect and
		   nothing happens, with nothing anywhere to explain it. */
		obs_log(LOG_ERROR, "twitch: could not start the connection thread (%d); the Twitch "
				   "feed will not work this session", rc);
		pthread_mutex_destroy(&t->lock);
		bfree(t);
		return NULL;
	}
	return t;
}

void ff_twitch_destroy(struct ff_twitch *t)
{
	if (!t)
		return;
	pthread_mutex_lock(&t->lock);
	t->running = false;
	pthread_mutex_unlock(&t->lock);
	pthread_join(t->thread, NULL);
	pthread_mutex_destroy(&t->lock);
	bfree(t);
}
