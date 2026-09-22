#include "ff-net.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <util/platform.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <errno.h>
#include <poll.h>
#endif

bool ff_url_parse(const char *url, struct ff_url *out)
{
	if (!url || !out)
		return false;
	memset(out, 0, sizeof *out);

	const char *p = strstr(url, "://");
	if (!p)
		return false;
	size_t slen = (size_t)(p - url);
	if (slen == 0 || slen >= sizeof out->scheme)
		return false;
	memcpy(out->scheme, url, slen);
	out->scheme[slen] = 0;

	if (!strcmp(out->scheme, "wss") || !strcmp(out->scheme, "https"))
		out->secure = true;
	else if (!strcmp(out->scheme, "ws") || !strcmp(out->scheme, "http"))
		out->secure = false;
	else
		return false;

	const char *h = p + 3;
	/* the authority ends at the first '/', '?' or '#'; everything after is the path */
	const char *end = h;
	while (*end && *end != '/' && *end != '?' && *end != '#')
		end++;
	const char *colon = NULL;
	const char *hstart = h, *hend = end;
	if (*h == '[') {
		/* An IPv6 literal is bracketed and full of colons (RFC 3986 s3.2.2). Scanning for the
		   last colon would cut the address in half and connect to whatever was left, which is
		   a wrong host rather than an error -- the worst outcome available here. */
		const char *close = h;
		while (close < end && *close != ']')
			close++;
		if (close == end)
			return false;
		hstart = h + 1;
		hend = close;
		if (close + 1 < end) {
			if (close[1] != ':')
				return false;
			colon = close + 1;
		}
	} else {
		for (const char *q = h; q < end; q++)
			if (*q == ':')
				colon = q;
		hend = colon ? colon : end;
	}
	size_t hlen = (size_t)(hend - hstart);
	if (hlen == 0 || hlen >= sizeof out->host)
		return false;
	memcpy(out->host, hstart, hlen);
	out->host[hlen] = 0;

	if (colon) {
		size_t plen = (size_t)(end - colon - 1);
		if (plen == 0 || plen >= sizeof out->port)
			return false;
		for (size_t i = 0; i < plen; i++)
			if (colon[1 + i] < '0' || colon[1 + i] > '9')
				return false;
		memcpy(out->port, colon + 1, plen);
		out->port[plen] = 0;
	} else {
		snprintf(out->port, sizeof out->port, "%s", out->secure ? "443" : "80");
	}

	/* An empty path is "/" -- a request line of "GET  HTTP/1.1" is not a request line. */
	if (*end == 0 || *end == '#')
		snprintf(out->path, sizeof out->path, "/");
	else if ((size_t)strlen(end) >= sizeof out->path)
		return false;
	else
		snprintf(out->path, sizeof out->path, "%s", end);
	return true;
}

struct ff_net {
	CURL *curl;
	curl_socket_t sock;
	struct ff_ws_conn conn;
};

static long net_send(void *ctx, const void *buf, size_t len)
{
	struct ff_net *n = ctx;
	size_t sent = 0;
	CURLcode rc = curl_easy_send(n->curl, buf, len, &sent);
	if (rc == CURLE_AGAIN)
		return 0;
	if (rc != CURLE_OK)
		return -1;
	return (long)sent;
}

static long net_recv(void *ctx, void *buf, size_t len)
{
	struct ff_net *n = ctx;
	size_t got = 0;
	CURLcode rc = curl_easy_recv(n->curl, buf, len, &got);
	if (rc == CURLE_AGAIN)
		return 0; /* nothing yet -- not an error, and not end of stream either */
	if (rc != CURLE_OK)
		return -1;
	/* curl reports a clean close as OK with zero bytes. That is the end of the connection, and
	   returning 0 here would mean "try again" forever on a socket that will never speak. */
	if (got == 0)
		return -1;
	return (long)got;
}

/* `writing` picks which readiness to wait for. It matters: the upgrade-send loop calls this to
   wait for the socket to accept data, and a POLLIN-only wait there degenerates into a plain sleep
   -- it works, but it is not what the name says, and "waits for the socket" would be a comment
   describing something the code does not do. */
static void net_wait(struct ff_net *n, int timeout_ms, bool writing)
{
	if (!n || n->sock == CURL_SOCKET_BAD)
		return;
#ifdef _WIN32
	fd_set s;
	FD_ZERO(&s);
	FD_SET(n->sock, &s);
	struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	select(0, writing ? NULL : &s, writing ? &s : NULL, NULL, &tv);
#else
	struct pollfd pfd = {.fd = n->sock, .events = writing ? POLLOUT : POLLIN};
	if (poll(&pfd, 1, timeout_ms) < 0 && errno != EINTR)
		/* A persistently failing poll (EBADF, EINVAL) would otherwise return instantly
		   every time and turn the caller's loop into a busy wait. Sleeping the interval
		   it asked for keeps the loop's timing honest even when the wait cannot work. */
		os_sleep_ms((uint32_t)(timeout_ms < 0 ? 0 : timeout_ms));
#endif
}

void ff_net_wait(struct ff_net *n, int timeout_ms)
{
	net_wait(n, timeout_ms, false);
}

struct ff_net *ff_net_ws_open(const char *url, char *err, size_t errcap)
{
	struct ff_url u;
	if (!ff_url_parse(url, &u)) {
		snprintf(err, errcap, "could not read the URL '%s'", url ? url : "(null)");
		return NULL;
	}

	struct ff_net *n = calloc(1, sizeof *n);
	if (!n) {
		snprintf(err, errcap, "out of memory");
		return NULL;
	}
	n->sock = CURL_SOCKET_BAD;
	n->curl = curl_easy_init();
	if (!n->curl) {
		snprintf(err, errcap, "curl_easy_init failed");
		free(n);
		return NULL;
	}

	/* curl has no ws:// scheme, so the connection is made as http(s) and upgraded by hand.
	   CONNECT_ONLY=1 stops curl after the TLS handshake and hands the socket over. */
	char curl_url[1024];
	snprintf(curl_url, sizeof curl_url, "%s://%s:%s%s", u.secure ? "https" : "http", u.host, u.port, u.path);
	curl_easy_setopt(n->curl, CURLOPT_URL, curl_url);
	curl_easy_setopt(n->curl, CURLOPT_CONNECT_ONLY, 1L);
	curl_easy_setopt(n->curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(n->curl, CURLOPT_NOSIGNAL, 1L);

	CURLcode rc = curl_easy_perform(n->curl);
	if (rc != CURLE_OK) {
		snprintf(err, errcap, "could not connect to %s: %s", u.host, curl_easy_strerror(rc));
		curl_easy_cleanup(n->curl);
		free(n);
		return NULL;
	}
	/* Checked, because the consequence of not having it is invisible: ff_net_wait returns
	   immediately on CURL_SOCKET_BAD, so the read loop stops sleeping and spins a core while
	   looking completely healthy from the outside -- reads and writes still work, through
	   curl. Better to fail the connection here, where there is something to say. */
	if (curl_easy_getinfo(n->curl, CURLINFO_ACTIVESOCKET, &n->sock) != CURLE_OK || n->sock == CURL_SOCKET_BAD) {
		snprintf(err, errcap, "connected to %s but could not get the socket to wait on", u.host);
		curl_easy_cleanup(n->curl);
		n->curl = NULL;
		free(n);
		return NULL;
	}

	struct ff_ws_io io = {.send = net_send, .recv = net_recv, .ctx = n};
	if (!ff_ws_conn_init(&n->conn, io)) {
		snprintf(err, errcap, "out of memory");
		ff_net_close(n);
		return NULL;
	}

	/* 16 random bytes, base64: RFC 6455 s4.1. The server's Sec-WebSocket-Accept is derived
	   from exactly these, which is what proves we are talking to a WebSocket server and not to
	   a proxy that answered for it. */
	uint8_t raw[16];
	if (!ff_ws_random(raw, sizeof raw)) {
		snprintf(err, errcap, "no random source for the handshake key");
		ff_net_close(n);
		return NULL;
	}
	static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char key[25];
	for (int i = 0; i < 5; i++) {
		uint32_t v = ((uint32_t)raw[i * 3] << 16) | ((uint32_t)raw[i * 3 + 1] << 8) | raw[i * 3 + 2];
		key[i * 4 + 0] = B64[(v >> 18) & 63];
		key[i * 4 + 1] = B64[(v >> 12) & 63];
		key[i * 4 + 2] = B64[(v >> 6) & 63];
		key[i * 4 + 3] = B64[v & 63];
	}
	uint32_t last = (uint32_t)raw[15] << 16;
	key[20] = B64[(last >> 18) & 63];
	key[21] = B64[(last >> 12) & 63];
	key[22] = '=';
	key[23] = '=';
	key[24] = 0;

	char req[1024];
	size_t rn = ff_ws_handshake_request(u.host, u.path, key, req, sizeof req);
	if (!rn) {
		snprintf(err, errcap, "the upgrade request would not fit");
		ff_net_close(n);
		return NULL;
	}
	size_t sent = 0;
	while (sent < rn) {
		long w = net_send(n, req + sent, rn - sent);
		if (w < 0) {
			snprintf(err, errcap, "the connection died sending the upgrade request");
			ff_net_close(n);
			return NULL;
		}
		if (w == 0)
			net_wait(n, 100, true);
		sent += (size_t)w;
	}

	/* Read until the headers are complete. Whatever arrives AFTER them is already the first
	   frame and is kept -- a server may put the 101 and a message in one packet, and throwing
	   the remainder away loses a message with no error anywhere. */
	char buf[8192];
	size_t have = 0;
	for (int spins = 0; spins < 300; spins++) {
		size_t hlen = ff_ws_handshake_check(buf, have, key, err, errcap);
		if (hlen == SIZE_MAX) {
			ff_net_close(n);
			return NULL;
		}
		if (hlen) {
			size_t extra = have - hlen;
			if (extra > FF_WS_CONN_BUF) {
				snprintf(err, errcap, "the server sent more than we can hold");
				ff_net_close(n);
				return NULL;
			}
			memcpy(n->conn.rx, buf + hlen, extra);
			n->conn.rx_len = extra;
			return n;
		}
		if (have >= sizeof buf) {
			snprintf(err, errcap, "the server's response headers do not end");
			ff_net_close(n);
			return NULL;
		}
		long r = net_recv(n, buf + have, sizeof buf - have);
		if (r < 0) {
			snprintf(err, errcap, "the connection died waiting for the upgrade");
			ff_net_close(n);
			return NULL;
		}
		if (r == 0)
			ff_net_wait(n, 100);
		have += (size_t)r;
	}
	snprintf(err, errcap, "the server never completed the upgrade");
	ff_net_close(n);
	return NULL;
}

struct ff_ws_conn *ff_net_conn(struct ff_net *n)
{
	return n ? &n->conn : NULL;
}

void ff_net_close(struct ff_net *n)
{
	if (!n)
		return;
	ff_ws_conn_free(&n->conn);
	if (n->curl)
		curl_easy_cleanup(n->curl);
	free(n);
}

/* ---------------------------------------------------------------- one-shot HTTP */

struct sink {
	char *p;
	size_t n;
};

static size_t on_body(char *data, size_t size, size_t nmemb, void *ctx)
{
	struct sink *s = ctx;
	size_t add = size * nmemb;
	/* a response larger than this is not something any of our calls returns, and growing
	   without a ceiling turns a hostile or broken endpoint into memory exhaustion */
	if (s->n + add > (4u << 20))
		return 0;
	char *bigger = realloc(s->p, s->n + add + 1);
	if (!bigger)
		return 0;
	s->p = bigger;
	memcpy(s->p + s->n, data, add);
	s->n += add;
	s->p[s->n] = 0;
	return add;
}

bool ff_http_request(const char *method, const char *url, const char *const *headers, size_t nh, const char *body,
		     struct ff_http_res *out, char *err, size_t errcap)
{
	if (!method || !url || !out) {
		snprintf(err, errcap, "bad arguments");
		return false;
	}
	memset(out, 0, sizeof *out);
	CURL *c = curl_easy_init();
	if (!c) {
		snprintf(err, errcap, "curl_easy_init failed");
		return false;
	}
	struct sink s = {0};
	struct curl_slist *hl = NULL;
	for (size_t i = 0; i < nh; i++) {
		/* curl_slist_append returns NULL on failure and does NOT free what it was given, so
		   assigning the result would leak the list AND silently drop every header -- the
		   Authorization line among them. Twitch would answer 401 and the message would say
		   the sign-in was rejected, sending the streamer to re-authorise for no reason. */
		struct curl_slist *next = curl_slist_append(hl, headers[i]);
		if (!next) {
			curl_slist_free_all(hl);
			curl_easy_cleanup(c);
			free(s.p);
			snprintf(err, errcap, "out of memory building the request headers");
			return false;
		}
		hl = next;
	}

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
	if (hl)
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
	if (body) {
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
	}

	CURLcode rc = curl_easy_perform(c);
	bool ok = rc == CURLE_OK;
	if (!ok)
		snprintf(err, errcap, "%s %s: %s", method, url, curl_easy_strerror(rc));
	else
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);

	if (hl)
		curl_slist_free_all(hl);
	curl_easy_cleanup(c);
	if (ok) {
		out->body = s.p;
		out->len = s.n;
	} else {
		free(s.p);
	}
	return ok;
}

void ff_http_res_free(struct ff_http_res *r)
{
	if (!r)
		return;
	free(r->body);
	r->body = NULL;
	r->len = 0;
}
