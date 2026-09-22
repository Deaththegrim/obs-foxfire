#pragma once
#include "ff-ws-conn.h"

#include <stdbool.h>
#include <stddef.h>

/* The transport: libcurl, and nothing above it.
 *
 * Why curl and not a WebSocket library: obs-deps already ships curl on every platform OBS builds
 * for, and curl's OWN WebSocket API is experimental, off by default, and absent from the libcurl
 * on this machine (measured -- curl_version_info() lists no ws or wss). CURLOPT_CONNECT_ONLY
 * hands over an established TLS connection and lets us talk on it, which is why ff-ws.c and
 * ff-ws-conn.c exist. See foxfire/research/alerts-transport.md.
 */

/* A URL split into the parts the handshake needs. `secure` decides http vs https for curl, which
   is what actually selects TLS -- curl has no ws:// scheme. */
struct ff_url {
	char scheme[8];
	char host[256];
	char port[8];
	char path[512];
	bool secure;
};

/* Splits a ws://, wss://, http:// or https:// URL. Returns false on anything it cannot read --
   including a missing host, which would otherwise become a connection to nowhere with a
   confusing error. Pure string work, so it is unit tested. */
bool ff_url_parse(const char *url, struct ff_url *out);

struct ff_net;

/* Connects, performs the WebSocket upgrade, and checks the response. On success the connection
   is ready for ff_ws_conn_poll/ff_ws_conn_send. Returns NULL and fills `err` otherwise. */
struct ff_net *ff_net_ws_open(const char *url, char *err, size_t errcap);

/* The connection to poll and send on. Valid until ff_net_close. */
struct ff_ws_conn *ff_net_conn(struct ff_net *n);

/* Sleeps until the socket has something to say or `timeout_ms` passes. Without this the read
   loop would spin a core: ff_ws_conn_poll returns 0 immediately when nothing has arrived, and on
   an idle EventSub connection nothing arrives for ten seconds at a time. */
void ff_net_wait(struct ff_net *n, int timeout_ms);

void ff_net_close(struct ff_net *n);

/* ---- one-shot HTTPS, for the device-code flow and the subscription calls ---- */

struct ff_http_res {
	long status;
	char *body; /* NUL-terminated for convenience; `len` is the real length */
	size_t len;
};

/* `headers` is an array of "Name: value" strings. Returns false only when the request could not
   be made at all -- an HTTP error status is a successful request with a status to look at, and
   conflating the two loses the response body that says why. */
bool ff_http_request(const char *method, const char *url, const char *const *headers, size_t nh, const char *body,
		     struct ff_http_res *out, char *err, size_t errcap);
void ff_http_res_free(struct ff_http_res *r);
