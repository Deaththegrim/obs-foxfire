#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RFC 6455 framing, and nothing else.
 *
 * No sockets, no TLS, no handshake -- those live with the transport. This is pure buffer work so
 * it can be tested against the RFC's own published example frames rather than against a server
 * that might be forgiving. That matters more here than it looks: a client frame MUST be masked
 * (RFC 6455 s5.3) and a server that receives an unmasked one "MUST close the connection", with no
 * error text and nothing in a log. Getting masking wrong does not misbehave -- it disconnects. */

enum ff_ws_opcode {
	FF_WS_CONT = 0x0,
	FF_WS_TEXT = 0x1,
	FF_WS_BINARY = 0x2,
	FF_WS_CLOSE = 0x8,
	FF_WS_PING = 0x9,
	FF_WS_PONG = 0xa,
};

/* Largest payload we will accept from the server. EventSub's notifications are small; a frame
   claiming gigabytes is either a bug or hostile, and the 64-bit length field lets it claim that. */
#define FF_WS_MAX_PAYLOAD (1u << 20)

struct ff_ws_msg {
	enum ff_ws_opcode op;
	bool fin;
	const uint8_t *payload; /* points INTO the caller's buffer; no copy, no ownership */
	size_t len;
};

/* Builds one masked client frame into `out`.
 *
 * `mask` is 4 bytes and must be unpredictable -- the RFC requires it to come from a strong source
 * because masking exists to stop a hostile client being steered into emitting bytes that look
 * like a request to an intermediary proxy, not to provide secrecy.
 *
 * Returns the number of bytes written, or 0 if `cap` is too small or the arguments are unusable.
 * Never writes a partial frame. */
size_t ff_ws_build(enum ff_ws_opcode op, const void *payload, size_t len, const uint8_t mask[4], uint8_t *out,
		   size_t cap);

/* How many bytes a complete frame at the front of `buf` occupies, filling `out`.
 *
 * Returns 0 when the buffer does not yet hold a whole frame -- the caller reads more and asks
 * again -- and SIZE_MAX when the frame is malformed or larger than FF_WS_MAX_PAYLOAD, which is
 * fatal for the connection. "Need more" and "this is broken" have to be different answers: a
 * parser that returns 0 for both waits forever on a connection that will never be valid. */
size_t ff_ws_parse(const uint8_t *buf, size_t len, struct ff_ws_msg *out);

/* The Sec-WebSocket-Accept value a server must return for a given Sec-WebSocket-Key: base64 of
   SHA-1 over the key concatenated with the RFC's fixed GUID. `out` needs 29 bytes + NUL.
   Checking it is what stops a plain HTTP endpoint (or a confused proxy) being mistaken for a
   WebSocket server. Returns false if the arguments are unusable. */
bool ff_ws_accept_for(const char *key_b64, char *out, size_t cap);
