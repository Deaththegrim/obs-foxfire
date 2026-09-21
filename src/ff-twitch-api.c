#include "ff-twitch-api.h"
#include "ff-net.h"
#include "ff-pack.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char ID_BASE[256] = "https://id.twitch.tv";
static char API_BASE[256] = "https://api.twitch.tv";

void ff_twitch_set_bases(const char *id_base, const char *api_base)
{
	snprintf(ID_BASE, sizeof ID_BASE, "%s", id_base ? id_base : "https://id.twitch.tv");
	snprintf(API_BASE, sizeof API_BASE, "%s", api_base ? api_base : "https://api.twitch.tv");
}

size_t ff_twitch_scopes(char *out, size_t cap)
{
	if (!out || !cap)
		return 0;
	out[0] = 0;
	size_t n = 0;
	for (size_t i = 0; i < FF_ES_SUB_COUNT; i++) {
		const char *sc = FF_ES_SUBS[i].scope;
		if (!sc || !*sc)
			continue; /* channel.raid needs none */
		/* Scopes repeat across subscription types -- three of ours want
		   channel:read:subscriptions -- and Twitch rejects a duplicated scope list. */
		char probe[128];
		snprintf(probe, sizeof probe, "%s", sc);
		bool already = false;
		for (const char *p = out; (p = strstr(p, probe)) != NULL; p++) {
			char before = p == out ? ' ' : p[-1];
			char after = p[strlen(probe)];
			if (before == ' ' && (after == ' ' || after == 0)) {
				already = true;
				break;
			}
		}
		if (already)
			continue;
		int w = snprintf(out + n, cap - n, "%s%s", n ? " " : "", sc);
		if (w <= 0 || (size_t)w >= cap - n) {
			/* A SHORTER list is the dangerous answer: the sign-in succeeds, and one
			   alert type quietly never fires because its scope was never asked for. */
			out[0] = 0;
			return 0;
		}
		n += (size_t)w;
	}
	return n;
}

/* Percent-encodes into a form body. A scope list has colons and spaces in it, and a device code
   is opaque -- sending either raw produces a 400 that reads like a credentials problem. */
static size_t form_escape(const char *s, char *out, size_t cap)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t n = 0;
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
		if (safe) {
			if (n + 1 >= cap)
				return 0;
			out[n++] = (char)c;
		} else {
			if (n + 3 >= cap)
				return 0;
			out[n++] = '%';
			out[n++] = HEX[c >> 4];
			out[n++] = HEX[c & 15];
		}
	}
	if (n >= cap)
		return 0;
	out[n] = 0;
	return n;
}

/* `status` is 0 when the request never completed -- no DNS, no route, no TLS, a timeout. That is
   NOT the same as Twitch answering with an error, and the caller has to be able to tell. */
static obs_data_t *post_form(const char *url, const char *body, long *status, char *err,
			     size_t errcap)
{
	if (status)
		*status = 0;
	const char *hdr[] = {"Content-Type: application/x-www-form-urlencoded"};
	struct ff_http_res res;
	if (!ff_http_request("POST", url, hdr, 1, body, &res, err, errcap))
		return NULL;
	if (status)
		*status = res.status;
	obs_data_t *d = res.body ? obs_data_create_from_json(res.body) : NULL;
	if (!d)
		snprintf(err, errcap, "%s answered %ld with something that is not JSON", url,
			 res.status);
	ff_http_res_free(&res);
	return d;
}

bool ff_twitch_device_start(const char *client_id, struct ff_twitch_device *out, char *err,
			    size_t errcap)
{
	if (!client_id || !*client_id || !out) {
		snprintf(err, errcap, "no client id set");
		return false;
	}
	memset(out, 0, sizeof *out);

	char scopes[512], esc_scopes[1536], esc_id[256];
	ff_twitch_scopes(scopes, sizeof scopes);
	if (!form_escape(scopes, esc_scopes, sizeof esc_scopes) ||
	    !form_escape(client_id, esc_id, sizeof esc_id)) {
		snprintf(err, errcap, "the request would not fit");
		return false;
	}
	char url[512], body[2048];
	snprintf(url, sizeof url, "%s/oauth2/device", ID_BASE);
	snprintf(body, sizeof body, "client_id=%s&scopes=%s", esc_id, esc_scopes);

	long status = 0;
	obs_data_t *d = post_form(url, body, &status, err, errcap);
	if (!d)
		return false;
	bool ok = false;
	if (status / 100 != 2) {
		snprintf(err, errcap, "Twitch refused the request (%ld): %s", status,
			 obs_data_get_string(d, "message"));
	} else {
		snprintf(out->device_code, sizeof out->device_code, "%s",
			 obs_data_get_string(d, "device_code"));
		snprintf(out->user_code, sizeof out->user_code, "%s",
			 obs_data_get_string(d, "user_code"));
		snprintf(out->verify_url, sizeof out->verify_url, "%s",
			 obs_data_get_string(d, "verification_uri"));
		out->interval = (int)obs_data_get_int(d, "interval");
		out->expires_in = (int)obs_data_get_int(d, "expires_in");
		/* Twitch documents 5 seconds; a 0 here would poll in a tight loop and be rate
		   limited into looking like a broken login. */
		if (out->interval < 1)
			out->interval = 5;
		if (!out->user_code[0] || !out->device_code[0]) {
			snprintf(err, errcap, "Twitch returned no code to show");
		} else {
			if (!out->verify_url[0])
				snprintf(out->verify_url, sizeof out->verify_url,
					 "https://www.twitch.tv/activate");
			ok = true;
		}
	}
	obs_data_release(d);
	return ok;
}

static void token_from(obs_data_t *d, struct ff_twitch_token *t)
{
	memset(t, 0, sizeof *t);
	snprintf(t->access, sizeof t->access, "%s", obs_data_get_string(d, "access_token"));
	snprintf(t->refresh, sizeof t->refresh, "%s", obs_data_get_string(d, "refresh_token"));
	snprintf(t->scopes, sizeof t->scopes, "%s", obs_data_get_string(d, "scope"));
	long long ttl = obs_data_get_int(d, "expires_in");
	/* Stored as an absolute moment. "Valid for four hours" stays true forever if the machine
	   sleeps, and the first sign of that is alerts stopping mid-stream. */
	t->expires_at = time(NULL) + (ttl > 0 ? (time_t)ttl : 3600);
}

enum ff_twitch_poll ff_twitch_device_poll(const char *client_id, const char *device_code,
					  struct ff_twitch_token *out, char *err, size_t errcap)
{
	if (!client_id || !device_code || !out) {
		snprintf(err, errcap, "nothing to poll with");
		return FF_TW_ERROR;
	}
	char esc_id[256], esc_dev[1024], scopes[512], esc_scopes[1536];
	ff_twitch_scopes(scopes, sizeof scopes);
	if (!form_escape(client_id, esc_id, sizeof esc_id) ||
	    !form_escape(device_code, esc_dev, sizeof esc_dev) ||
	    !form_escape(scopes, esc_scopes, sizeof esc_scopes)) {
		snprintf(err, errcap, "the request would not fit");
		return FF_TW_ERROR;
	}
	char url[512], body[3072];
	snprintf(url, sizeof url, "%s/oauth2/token", ID_BASE);
	snprintf(body, sizeof body,
		 "client_id=%s&scopes=%s&device_code=%s"
		 "&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
		 esc_id, esc_scopes, esc_dev);

	long status = 0;
	obs_data_t *d = post_form(url, body, &status, err, errcap);
	if (!d)
		return FF_TW_ERROR;

	enum ff_twitch_poll r;
	const char *msg = obs_data_get_string(d, "message");
	/* Classified on the STATUS first and the message second. These strings are human-readable
	   text from a server we do not control: a 503 "Service temporarily unavailable" matches
	   none of them and would be read as a hard failure, abandoning a sign-in while the streamer
	   is still walking to their phone. A 5xx is always "try again". */
	if (status / 100 == 5) {
		snprintf(err, errcap, "Twitch is having trouble (%ld); still waiting", status);
		obs_data_release(d);
		return FF_TW_PENDING;
	}
	if (status / 100 == 2 && obs_data_get_string(d, "access_token")[0]) {
		token_from(d, out);
		r = FF_TW_GOT_TOKEN;
	} else if (msg && strstr(msg, "authorization_pending")) {
		/* The expected answer for most of this flow -- the streamer has not typed the code
		   yet. Reported as PENDING, not as an error, or the UI would show a failure every
		   five seconds while they are still walking to their phone. */
		r = FF_TW_PENDING;
	} else if (msg && strstr(msg, "slow_down")) {
		r = FF_TW_SLOW_DOWN;
	} else if (msg && (strstr(msg, "expired") || strstr(msg, "invalid device code"))) {
		snprintf(err, errcap, "the code expired before it was used");
		r = FF_TW_EXPIRED;
	} else if (msg && strstr(msg, "denied")) {
		snprintf(err, errcap, "authorisation was refused");
		r = FF_TW_DENIED;
	} else {
		snprintf(err, errcap, "Twitch answered %ld: %s", status, msg ? msg : "(no message)");
		r = FF_TW_ERROR;
	}
	obs_data_release(d);
	return r;
}

enum ff_refresh_result ff_twitch_refresh(const char *client_id, const char *refresh_token,
					 struct ff_twitch_token *out, char *err, size_t errcap)
{
	if (!client_id || !refresh_token || !*refresh_token || !out) {
		snprintf(err, errcap, "no refresh token stored");
		return FF_REFRESH_REJECTED;
	}
	char esc_id[256], esc_rt[1024];
	if (!form_escape(client_id, esc_id, sizeof esc_id) ||
	    !form_escape(refresh_token, esc_rt, sizeof esc_rt)) {
		snprintf(err, errcap, "the request would not fit");
		return FF_REFRESH_REJECTED;
	}
	char url[512], body[2048];
	snprintf(url, sizeof url, "%s/oauth2/token", ID_BASE);
	snprintf(body, sizeof body, "client_id=%s&grant_type=refresh_token&refresh_token=%s",
		 esc_id, esc_rt);
	long status = 0;
	obs_data_t *d = post_form(url, body, &status, err, errcap);
	if (!d) {
		/* status 0 means the request never completed. Treating that as a dead token is how
		   a router reboot costs a streamer their sign-in. */
		if (status == 0) {
			snprintf(err, errcap, "could not reach Twitch to refresh the sign-in");
			return FF_REFRESH_UNREACHABLE;
		}
		snprintf(err, errcap, "Twitch answered %ld with something that is not JSON", status);
		return FF_REFRESH_UNREACHABLE;
	}
	enum ff_refresh_result r;
	if (status / 100 == 2 && obs_data_get_string(d, "access_token")[0]) {
		token_from(d, out);
		r = FF_REFRESH_OK;
	} else if (status / 100 == 4) {
		/* A refresh token stops working when the streamer disconnects the app or changes
		   their password. That is a "sign in again", not a retry, and saying so is the
		   difference between one click and a support message. Only a 4xx means this. */
		snprintf(err, errcap, "the saved sign-in is no longer valid (%ld): %s", status,
			 obs_data_get_string(d, "message"));
		r = FF_REFRESH_REJECTED;
	} else {
		/* a 5xx, or a 2xx with no token in it: Twitch's problem, not the token's */
		snprintf(err, errcap, "Twitch could not refresh the sign-in right now (%ld): %s",
			 status, obs_data_get_string(d, "message"));
		r = FF_REFRESH_UNREACHABLE;
	}
	obs_data_release(d);
	return r;
}

bool ff_twitch_user_id(const char *client_id, const char *access, char *id, size_t idcap,
		       char *login, size_t logincap, char *err, size_t errcap)
{
	if (!client_id || !access || !id) {
		snprintf(err, errcap, "not signed in");
		return false;
	}
	char url[512], auth[600], cid[400];
	snprintf(url, sizeof url, "%s/helix/users", API_BASE);
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", access);
	snprintf(cid, sizeof cid, "Client-Id: %s", client_id);
	const char *hdr[] = {auth, cid};

	struct ff_http_res res;
	if (!ff_http_request("GET", url, hdr, 2, NULL, &res, err, errcap))
		return false;
	bool ok = false;
	obs_data_t *d = res.body ? obs_data_create_from_json(res.body) : NULL;
	if (!d) {
		snprintf(err, errcap, "helix/users answered %ld with something that is not JSON",
			 res.status);
	} else {
		obs_data_array_t *arr = obs_data_get_array(d, "data");
		obs_data_t *u = arr && obs_data_array_count(arr) ? obs_data_array_item(arr, 0) : NULL;
		if (!u) {
			snprintf(err, errcap, "helix/users answered %ld with no user", res.status);
		} else {
			snprintf(id, idcap, "%s", obs_data_get_string(u, "id"));
			if (login)
				snprintf(login, logincap, "%s", obs_data_get_string(u, "login"));
			ok = id[0] != 0;
			if (!ok)
				snprintf(err, errcap, "helix/users returned a user with no id");
			obs_data_release(u);
		}
		obs_data_array_release(arr);
		obs_data_release(d);
	}
	ff_http_res_free(&res);
	return ok;
}

bool ff_twitch_subscribe(const char *client_id, const char *access, const struct ff_es_sub *sub,
			 const char *broadcaster_id, const char *session_id, long *status,
			 char *err, size_t errcap)
{
	if (!client_id || !access || !sub || !broadcaster_id || !session_id) {
		snprintf(err, errcap, "not enough to subscribe with");
		return false;
	}
	/* channel.follow v2's condition needs moderator_user_id as well, and the broadcaster is a
	   moderator of their own channel. Omitting it is refused at subscribe time, which is the
	   documented trap -- the alert type then simply never fires. */
	char cond[512];
	if (sub->needs_moderator)
		snprintf(cond, sizeof cond,
			 "{\"broadcaster_user_id\":\"%s\",\"moderator_user_id\":\"%s\"}",
			 broadcaster_id, broadcaster_id);
	else if (!strcmp(sub->type, "channel.raid"))
		/* a raid's condition is about who RECEIVES it */
		snprintf(cond, sizeof cond, "{\"to_broadcaster_user_id\":\"%s\"}", broadcaster_id);
	else
		snprintf(cond, sizeof cond, "{\"broadcaster_user_id\":\"%s\"}", broadcaster_id);

	char body[1536];
	snprintf(body, sizeof body,
		 "{\"type\":\"%s\",\"version\":\"%s\",\"condition\":%s,"
		 "\"transport\":{\"method\":\"websocket\",\"session_id\":\"%s\"}}",
		 sub->type, sub->version, cond, session_id);

	char url[512], auth[600], cid[400];
	snprintf(url, sizeof url, "%s/helix/eventsub/subscriptions", API_BASE);
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", access);
	snprintf(cid, sizeof cid, "Client-Id: %s", client_id);
	const char *hdr[] = {auth, cid, "Content-Type: application/json"};

	struct ff_http_res res;
	if (!ff_http_request("POST", url, hdr, 3, body, &res, err, errcap))
		return false;
	if (status)
		*status = res.status;
	/* 409 Conflict means this exact subscription already exists -- which is what Twitch returns
	   after a session_reconnect carried them across, and after OBS restarts inside the grace
	   window. It is the state we wanted, so it is a success. Counting it as a refusal is how a
	   routine reconnect turns into "Twitch refused every alert type". */
	bool ok = res.status / 100 == 2 || res.status == 409;
	if (!ok) {
		obs_data_t *d = res.body ? obs_data_create_from_json(res.body) : NULL;
		const char *msg = d ? obs_data_get_string(d, "message") : NULL;
		/* Named per subscription, because the usual cause is one missing scope and the
		   symptom is one alert type that never arrives while the others do. */
		snprintf(err, errcap, "%s was refused (%ld)%s%s", sub->type, res.status,
			 msg && *msg ? ": " : "", msg && *msg ? msg : "");
		obs_data_release(d);
	}
	ff_http_res_free(&res);
	return ok;
}

/* ---------------------------------------------------------------- token storage */

static bool token_path(const char *module_path, char *out, size_t cap)
{
	return ff_shared_config_path(module_path, "twitch.json", out, cap);
}

bool ff_twitch_token_save(const char *module_path, const struct ff_twitch_token *t)
{
	char path[1024];
	if (!t || !token_path(module_path, path, sizeof path)) {
		obs_log(LOG_WARNING,
			"twitch: no usable config path for the sign-in (module path '%s')",
			module_path ? module_path : "(null)");
		return false;
	}

	obs_data_t *d = obs_data_create();
	/* the ACCESS token is deliberately not stored: it lasts about four hours, and the refresh
	   token can always mint another. One fewer credential on disk. */
	obs_data_set_string(d, "refresh", t->refresh);
	obs_data_set_string(d, "scopes", t->scopes);
	bool ok = obs_data_save_json_safe(d, path, "tmp", "bak");
	obs_data_release(d);
	if (ok) {
		/* 0600 AFTER the write: obs_data_save_json_safe creates with the process umask,
		   which on a default Linux desktop is world-readable. A refresh token is a
		   credential -- anything that can read it can post as the streamer. */
#ifndef _WIN32
		if (chmod(path, S_IRUSR | S_IWUSR) != 0)
			obs_log(LOG_WARNING,
				"twitch: could not restrict permissions on %s; it holds a "
				"credential and is readable by other accounts on this machine",
				path);
#endif
	}
	return ok;
}

bool ff_twitch_token_load(const char *module_path, struct ff_twitch_token *t)
{
	char path[1024];
	if (!t || !token_path(module_path, path, sizeof path))
		return false;
	memset(t, 0, sizeof *t);
	obs_data_t *d = obs_data_create_from_json_file(path);
	if (!d)
		return false;
	snprintf(t->refresh, sizeof t->refresh, "%s", obs_data_get_string(d, "refresh"));
	snprintf(t->scopes, sizeof t->scopes, "%s", obs_data_get_string(d, "scopes"));
	obs_data_release(d);
	return t->refresh[0] != 0;
}

void ff_twitch_token_forget(const char *module_path)
{
	char path[1024];
	if (token_path(module_path, path, sizeof path))
		remove(path);
}
