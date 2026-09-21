#include "ff-twitch.h"
#include "ff-net.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <pthread.h>
#include <stdarg.h>
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
	void *ctx;
};

static void set_state(struct ff_twitch *t, enum ff_twitch_state s, const char *fmt, ...)
{
	pthread_mutex_lock(&t->lock);
	t->state = s;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(t->line, sizeof t->line, fmt, ap);
	va_end(ap);
	if (s != FF_TWS_SIGNING_IN) {
		t->code[0] = 0;
		t->verify_url[0] = 0;
	}
	pthread_mutex_unlock(&t->lock);
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
	set_state(t, FF_TWS_SIGNING_IN, obs_module_text("Foxfire.Twitch.EnterCode"), dev.user_code,
		  dev.verify_url);

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
			if (!ff_twitch_token_save(t->module_path, &tok))
				obs_log(LOG_WARNING, "twitch: signed in but could not save the token");
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
	if (!ff_twitch_refresh(cid, saved.refresh, &tok, err, sizeof err)) {
		/* The saved sign-in stopped working -- disconnected in Twitch's settings, or a
		   password change. Retrying cannot fix it, so the token goes and the panel says
		   sign in again rather than looping forever on RETRYING. */
		ff_twitch_token_forget(t->module_path);
		set_state(t, FF_TWS_FAILED, "%s", err);
		return false;
	}
	/* Twitch rotates the refresh token on use; keeping the old one works until it does not. */
	if (tok.refresh[0])
		ff_twitch_token_save(t->module_path, &tok);

	char uid[64] = {0}, login[64] = {0};
	if (!ff_twitch_user_id(cid, tok.access, uid, sizeof uid, login, sizeof login, err,
			       sizeof err)) {
		set_state(t, FF_TWS_RETRYING, "%s", err);
		return false;
	}

	char url[1024];
	snprintf(url, sizeof url, "%s", FF_TW_URL);
	bool ran_long = false;

	for (;;) { /* one pass per session; a session_reconnect starts another with a new URL */
		set_state(t, FF_TWS_CONNECTING, obs_module_text("Foxfire.Twitch.Connecting"), login);
		struct ff_net *net = ff_net_ws_open(url, err, sizeof err);
		if (!net) {
			set_state(t, FF_TWS_RETRYING, "%s", err);
			return ran_long;
		}

		struct ff_es es;
		memset(&es, 0, sizeof es);
		time_t last = time(NULL);
		time_t started = last;
		bool subscribed = false;
		bool reconnecting = false;

		while (reading(t, &t->running) && reading(t, &t->enabled)) {
			struct ff_ws_msg m;
			int r = ff_ws_conn_poll(ff_net_conn(net), &m);
			if (r < 0) {
				set_state(t, FF_TWS_RETRYING, "%s", ff_net_conn(net)->err);
				break;
			}
			if (r == 0) {
				if (ff_twitch_is_stale(last, es.keepalive_secs, time(NULL))) {
					set_state(t, FF_TWS_RETRYING, "%s",
						  obs_module_text("Foxfire.Twitch.WentQuiet"));
					break;
				}
				ff_net_wait(net, 200);
				continue;
			}
			last = time(NULL);

			/* the payload is not NUL-terminated: it points into the read buffer */
			char *json = malloc(m.len + 1);
			if (!json)
				break;
			memcpy(json, m.payload, m.len);
			json[m.len] = 0;

			struct ff_alert_event ev;
			enum ff_es_result res = ff_es_handle(&es, json, &ev);
			free(json);

			if (res == FF_ES_WELCOME && !subscribed) {
				int ok = 0;
				for (size_t i = 0; i < FF_ES_SUB_COUNT; i++) {
					long status = 0;
					if (ff_twitch_subscribe(cid, tok.access, &FF_ES_SUBS[i], uid,
								es.session_id, &status, err,
								sizeof err))
						ok++;
					else
						/* One refused subscription is not a dead
						   connection -- a missing scope takes out one alert
						   type and the rest still work. Named, so the reason
						   the follows never arrive is findable. */
						obs_log(LOG_WARNING, "twitch: %s", err);
				}
				subscribed = true;
				if (!ok) {
					set_state(t, FF_TWS_FAILED, "%s",
						  obs_module_text("Foxfire.Twitch.NoSubs"));
					ff_net_close(net);
					return ran_long;
				}
				set_state(t, FF_TWS_LIVE, obs_module_text("Foxfire.Twitch.Live"), ok,
					  (int)FF_ES_SUB_COUNT, login);
			} else if (res == FF_ES_WELCOME) {
				/* the welcome on a RECONNECT session: the subscriptions moved with
				   it, so asking for them again would duplicate every alert */
				set_state(t, FF_TWS_LIVE, obs_module_text("Foxfire.Twitch.Live"),
					  (int)FF_ES_SUB_COUNT, (int)FF_ES_SUB_COUNT, login);
			} else if (res == FF_ES_EVENT) {
				if (t->cb)
					t->cb(t->ctx, &ev);
			} else if (res == FF_ES_RECONNECT) {
				/* Thirty seconds to move, and the old connection keeps delivering
				   until the new one is welcomed -- so this breaks out and reopens
				   rather than tearing down first. */
				snprintf(url, sizeof url, "%s", es.reconnect_url);
				reconnecting = true;
				break;
			} else if (res == FF_ES_REVOKED) {
				obs_log(LOG_WARNING, "twitch: %s", es.note);
				set_state(t, FF_TWS_FAILED, "%s", es.note);
			} else if (res == FF_ES_BAD) {
				obs_log(LOG_WARNING, "twitch: %s", es.err);
			} else if (es.note[0]) {
				obs_log(LOG_INFO, "twitch: %s", es.note);
			}

			if (time(NULL) - started > 60)
				ran_long = true;
		}

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
		bool blocked = t->state == FF_TWS_FAILED;
		pthread_mutex_unlock(&t->lock);
		if (blocked) {
			/* FAILED means a person has to do something. Retrying would hide that
			   behind a spinner and burn requests for as long as OBS is open. */
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
		if (stop)
			continue;
		int wait = ff_twitch_backoff_secs(attempt);
		set_state(t, FF_TWS_RETRYING, obs_module_text("Foxfire.Twitch.Retrying"), wait);
		nap(t, wait);
	}
	return NULL;
}

struct ff_twitch *ff_twitch_create(ff_twitch_event_cb cb, void *ctx, const char *module_path)
{
	struct ff_twitch *t = bzalloc(sizeof *t);
	pthread_mutex_init(&t->lock, NULL);
	t->cb = cb;
	t->ctx = ctx;
	t->running = true;
	snprintf(t->module_path, sizeof t->module_path, "%s", module_path ? module_path : "");
	snprintf(t->line, sizeof t->line, "%s", obs_module_text("Foxfire.Twitch.NotSignedIn"));
	if (pthread_create(&t->thread, NULL, worker, t) != 0) {
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
