#pragma once
#include "ff-eventsub.h"

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Talking to Twitch's HTTP side: getting a token, and asking for the subscriptions the WebSocket
 * connection will then receive.
 *
 * All of it goes through ff_http_request, so a test points the base URLs at a local server and the
 * SAME code runs -- there is no test-only path here. That matters more than usual: this is the
 * part nobody can exercise by hand without a real Twitch account and a real stream.
 *
 * The Device Code Grant is used because it needs no client secret. A GPL plugin ships its source,
 * so any secret in it is public the moment it is released; a public client with no secret is the
 * only honest option. See foxfire/research/twitch-eventsub.md.
 */

struct ff_twitch_device {
	char device_code[512]; /* ours, secret-ish, used to poll */
	char user_code[32];    /* the short code the streamer types */
	char verify_url[256];  /* where they type it */
	int interval;          /* seconds between polls; polling faster is rate-limited */
	int expires_in;        /* seconds until the code is dead */
};

struct ff_twitch_token {
	char access[512];
	char refresh[512];
	time_t expires_at; /* absolute, so a token is not "valid for 4 hours" forever after a sleep */
	char scopes[512];
};

enum ff_twitch_poll {
	FF_TW_PENDING = 0, /* the streamer has not finished authorising yet */
	FF_TW_GOT_TOKEN,
	FF_TW_SLOW_DOWN, /* we polled too fast; wait longer, do not treat as an error */
	FF_TW_DENIED,    /* they said no */
	FF_TW_EXPIRED,   /* the code timed out */
	FF_TW_ERROR,
};

/* The scopes the subscriptions in FF_ES_SUBS need, space separated, built from that list rather
   than written out again -- two lists of scopes drift, and the symptom is one alert type silently
   never arriving. Returns the number of characters written, or 0 if they did not all fit.
   Truncating would ask for FEWER scopes than the code needs, and that surfaces much later as
   "some alert types never fire" -- so it refuses instead. */
size_t ff_twitch_scopes(char *out, size_t cap);

bool ff_twitch_device_start(const char *client_id, struct ff_twitch_device *out, char *err, size_t errcap);
enum ff_twitch_poll ff_twitch_device_poll(const char *client_id, const char *device_code, struct ff_twitch_token *out,
					  char *err, size_t errcap);
/* Why this is not a bool: "the token is dead" and "we could not reach Twitch" are different
   answers, and collapsing them means a Wi-Fi blip DELETES a perfectly good refresh token and the
   streamer has to sign in again for a router reboot. Only FF_REFRESH_REJECTED may forget it. */
enum ff_refresh_result {
	FF_REFRESH_OK = 0,
	FF_REFRESH_REJECTED,    /* Twitch answered, and said no. The saved sign-in really is dead. */
	FF_REFRESH_UNREACHABLE, /* no answer at all: DNS, TLS, timeout, a captive portal, a 502 */
};
enum ff_refresh_result ff_twitch_refresh(const char *client_id, const char *refresh_token, struct ff_twitch_token *out,
					 char *err, size_t errcap);

/* Who the token belongs to. EventSub conditions need the broadcaster's numeric id, not the name. */
bool ff_twitch_user_id(const char *client_id, const char *access, char *id, size_t idcap, char *login, size_t logincap,
		       char *err, size_t errcap);

/* Asks for one subscription on an open WebSocket session. Returns false with `err` set; a 401 is
   reported as such so the caller can refresh rather than giving up. */
bool ff_twitch_subscribe(const char *client_id, const char *access, const struct ff_es_sub *sub,
			 const char *broadcaster_id, const char *session_id, long *status, char *err, size_t errcap);

/* Where the refresh token lives. A refresh token is a credential: it goes in the plugin's own
   config directory with 0600, never in a scene collection, which streamers share and back up. */
bool ff_twitch_token_save(const char *module_path, const struct ff_twitch_token *t);
bool ff_twitch_token_load(const char *module_path, struct ff_twitch_token *t);
void ff_twitch_token_forget(const char *module_path);

/* Points the ID and API hosts somewhere else. For tests only -- production never calls it, so the
   default is Twitch and a test that forgets to redirect talks to Twitch and fails loudly rather
   than silently passing against a mock. */
void ff_twitch_set_bases(const char *id_base, const char *api_base);
