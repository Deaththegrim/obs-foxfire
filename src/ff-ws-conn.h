#pragma once
#include "ff-ws.h"

/* A WebSocket CONNECTION: the HTTP upgrade, and the read loop that turns a byte stream into
 * messages. ff-ws.c is the framing and knows nothing about sockets; this knows nothing about TLS.
 *
 * The socket itself arrives as two function pointers (`struct ff_ws_io`), which is what makes the
 * hard part testable. The hard part is not "can it connect" -- it is everything that happens when
 * the bytes do not arrive the way the happy path assumes: a frame split across two reads, the 101
 * response and the first frame landing in the SAME read, a ping that must be answered inside the
 * read loop, a plain HTTP 200 from a proxy that intercepted the request. Every one of those is
 * driven here from a scripted buffer, offline, with no Twitch and no network.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Return >0 bytes moved, 0 for "nothing right now, try again", or <0 for a dead connection.
   Deliberately NOT errno-shaped: the curl implementation has its own codes and the test has none,
   so the contract is three cases and no shared error space. */
struct ff_ws_io {
	long (*send)(void *ctx, const void *buf, size_t len);
	long (*recv)(void *ctx, void *buf, size_t len);
	void *ctx;
};

/* Big enough for EventSub's largest notification plus a frame header, and the hard ceiling on how
   much unparsed data is held for one message. */
#define FF_WS_CONN_BUF (FF_WS_MAX_PAYLOAD + 16u)

struct ff_ws_conn {
	struct ff_ws_io io;
	uint8_t *rx; /* bytes read but not yet parsed into frames */
	size_t rx_len;
	/* A message may arrive as several frames (RFC 6455 s5.4), so frames are reassembled here
	   and only a WHOLE message is handed up. Handing up the pieces would push that job onto
	   every caller, and a JSON parser fed half a notification fails in a way that reads as
	   "Twitch sent us garbage". */
	uint8_t *msg;
	size_t msg_len;
	enum ff_ws_opcode msg_op;
	bool in_msg;
	bool open;    /* the handshake completed and no close has been seen */
	bool closing; /* a close was received or sent; no more application messages */
	uint16_t close_code;
	char err[256];
};

/* Allocates the buffers and takes the io. Returns false if it could not allocate. */
bool ff_ws_conn_init(struct ff_ws_conn *c, struct ff_ws_io io);
void ff_ws_conn_free(struct ff_ws_conn *c);

/* Builds the upgrade request. `key_b64` is the caller's 16 random bytes, base64'd -- passed in
   rather than generated here so a test can use the RFC's own key and compare the bytes exactly.
   Returns the length written, or 0 if it would not fit. */
size_t ff_ws_handshake_request(const char *host, const char *path, const char *key_b64, char *out, size_t cap);

/* Checks a server's response to that request.
 *
 * Returns the number of bytes the RESPONSE HEADERS occupy, so the caller can keep whatever came
 * after them -- a server is entitled to put the 101 and the first frame in one packet, and a
 * reader that discards its buffer after the handshake loses that frame with no error anywhere.
 * 0 means the headers are not complete yet; SIZE_MAX means this is not a WebSocket server. */
size_t ff_ws_handshake_check(const char *resp, size_t len, const char *key_b64, char *err, size_t errcap);

/* Fills `out` with 16 unpredictable bytes. Used for the handshake key and for every frame mask;
   RFC 6455 s5.3 requires the mask to come from a strong source. Returns false if it could not. */
bool ff_ws_random(uint8_t *out, size_t n);

/* Sends one application message, masked, as a client must. */
bool ff_ws_conn_send(struct ff_ws_conn *c, enum ff_ws_opcode op, const void *payload, size_t len);

/* Reads what is available and reports ONE application message if a whole one has arrived.
 *
 *   1  `out` holds a TEXT or BINARY message; its payload points into the connection's buffer and
 *      is valid until the next call.
 *   0  nothing complete yet -- ping/pong and close handling may still have happened.
 *  -1  the connection is finished (protocol error, close received, or the socket died). `c->err`
 *      says which.
 *
 * Ping is answered here rather than handed up: a pong that waits on the caller's event loop is a
 * pong that arrives after the server has given up on us. */
int ff_ws_conn_poll(struct ff_ws_conn *c, struct ff_ws_msg *out);

/* Sends a close frame with `code`. Safe to call on an already-closing connection. */
void ff_ws_conn_close(struct ff_ws_conn *c, uint16_t code);
