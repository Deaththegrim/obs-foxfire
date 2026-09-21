#include "ff-ws-conn.h"

#include <stdarg.h>
#include <stdio.h>
#include <util/platform.h> /* os_sleep_ms */
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

bool ff_ws_random(uint8_t *out, size_t n)
{
	if (!out || !n)
		return false;
#ifdef _WIN32
	return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
	/* /dev/urandom rather than rand(): the mask has to be unpredictable (RFC 6455 s5.3), and
	   this is also the handshake key. Read in a loop -- a short read from urandom is rare but
	   is not an error, and treating it as one would fail a connection for no reason. */
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, out + got, n - got);
		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			close(fd);
			return false;
		}
		got += (size_t)r;
	}
	close(fd);
	return true;
#endif
}

size_t ff_ws_handshake_request(const char *host, const char *path, const char *key_b64,
			       char *out, size_t cap)
{
	if (!host || !path || !key_b64 || !out)
		return 0;
	/* Version 13 is the only version RFC 6455 defines. Connection and Upgrade are matched
	   case-insensitively by servers but are sent in the RFC's own casing. */
	int n = snprintf(out, cap,
			 "GET %s HTTP/1.1\r\n"
			 "Host: %s\r\n"
			 "Upgrade: websocket\r\n"
			 "Connection: Upgrade\r\n"
			 "Sec-WebSocket-Key: %s\r\n"
			 "Sec-WebSocket-Version: 13\r\n"
			 "\r\n",
			 path, host, key_b64);
	if (n <= 0 || (size_t)n >= cap)
		return 0;
	return (size_t)n;
}

/* Case-insensitive search for `name: value` in a header block, returning the value's bounds.
   Header names are case-insensitive (RFC 7230 s3.2) and servers do vary the casing. */
static bool header_value(const char *resp, size_t len, const char *name, const char **val,
			 size_t *vlen)
{
	size_t nlen = strlen(name);
	size_t i = 0;
	/* skip the status line */
	while (i < len && resp[i] != '\n')
		i++;
	if (i < len)
		i++;
	while (i < len) {
		size_t start = i;
		while (i < len && resp[i] != '\n')
			i++;
		size_t end = i; /* exclusive, may include a trailing \r */
		if (end > start && resp[end - 1] == '\r')
			end--;
		if (end == start)
			return false; /* blank line: end of headers */
		if (end - start > nlen && resp[start + nlen] == ':') {
			size_t k = 0;
			for (; k < nlen; k++) {
				char a = resp[start + k], b = name[k];
				if (a >= 'A' && a <= 'Z')
					a = (char)(a - 'A' + 'a');
				if (b >= 'A' && b <= 'Z')
					b = (char)(b - 'A' + 'a');
				if (a != b)
					break;
			}
			if (k == nlen) {
				size_t v = start + nlen + 1;
				while (v < end && (resp[v] == ' ' || resp[v] == '\t'))
					v++;
				*val = resp + v;
				*vlen = end - v;
				return true;
			}
		}
		if (i < len)
			i++;
	}
	return false;
}

static bool value_is(const char *v, size_t vlen, const char *want)
{
	size_t w = strlen(want);
	if (vlen != w)
		return false;
	for (size_t i = 0; i < w; i++) {
		char a = v[i], b = want[i];
		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = (char)(b - 'A' + 'a');
		if (a != b)
			return false;
	}
	return true;
}

/* Is `want` one of the comma-separated tokens in this header value? */
static bool value_has(const char *v, size_t vlen, const char *want)
{
	size_t i = 0;
	while (i < vlen) {
		while (i < vlen && (v[i] == ' ' || v[i] == '\t' || v[i] == ','))
			i++;
		size_t start = i;
		while (i < vlen && v[i] != ',')
			i++;
		size_t end = i;
		while (end > start && (v[end - 1] == ' ' || v[end - 1] == '\t'))
			end--;
		if (value_is(v + start, end - start, want))
			return true;
	}
	return false;
}

static void say(char *err, size_t cap, const char *fmt, ...)
{
	if (!err || !cap)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, cap, fmt, ap);
	va_end(ap);
}

size_t ff_ws_handshake_check(const char *resp, size_t len, const char *key_b64, char *err,
			     size_t errcap)
{
	if (!resp || !key_b64) {
		say(err, errcap, "no response to check");
		return SIZE_MAX;
	}
	/* The headers end at the first blank line. Until that arrives there is nothing to judge --
	   and judging a partial response would reject a server for the crime of being slow. */
	const char *end = NULL;
	for (size_t i = 0; i + 3 < len; i++)
		if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
			end = resp + i + 4;
			break;
		}
	if (!end) {
		/* A server that will never send a blank line would otherwise hang us forever. */
		if (len > 8192) {
			say(err, errcap, "no end of headers after %zu bytes; not an HTTP server", len);
			return SIZE_MAX;
		}
		return 0;
	}
	size_t hlen = (size_t)(end - resp);

	/* 101 and nothing else. A 200 is the case that matters: a captive portal or a proxy that
	   answered on the server's behalf returns a perfectly valid HTTP response, and a client
	   that only checks for "some response" would then read HTML as WebSocket frames. */
	if (hlen < 12 || strncmp(resp, "HTTP/1.", 7) != 0) {
		say(err, errcap, "response is not HTTP");
		return SIZE_MAX;
	}
	int status = atoi(resp + 9);
	if (status != 101) {
		say(err, errcap, "server answered %d, not 101 Switching Protocols", status);
		return SIZE_MAX;
	}

	const char *v;
	size_t vlen;
	if (!header_value(resp, hlen, "Upgrade", &v, &vlen) || !value_is(v, vlen, "websocket")) {
		say(err, errcap, "no 'Upgrade: websocket' header");
		return SIZE_MAX;
	}
	/* `Connection` is a COMMA-SEPARATED LIST (RFC 7230 s6.1), and a proxy in the path really
	   does send "keep-alive, Upgrade". Demanding the whole value equal "Upgrade" refuses a
	   correct server, which costs more than letting a wrong one through -- the accept-value
	   check below is what actually proves this is a WebSocket server. */
	if (!header_value(resp, hlen, "Connection", &v, &vlen) || !value_has(v, vlen, "Upgrade")) {
		say(err, errcap, "no 'Connection: Upgrade' header");
		return SIZE_MAX;
	}
	if (!header_value(resp, hlen, "Sec-WebSocket-Accept", &v, &vlen)) {
		say(err, errcap, "no Sec-WebSocket-Accept header");
		return SIZE_MAX;
	}
	char want[64];
	if (!ff_ws_accept_for(key_b64, want, sizeof want)) {
		say(err, errcap, "could not compute the expected accept value");
		return SIZE_MAX;
	}
	if (vlen != strlen(want) || memcmp(v, want, vlen) != 0) {
		say(err, errcap, "Sec-WebSocket-Accept does not match the key we sent");
		return SIZE_MAX;
	}
	return hlen;
}

bool ff_ws_conn_send(struct ff_ws_conn *c, enum ff_ws_opcode op, const void *payload, size_t len)
{
	if (!c || !c->io.send)
		return false;
	uint8_t mask[4];
	if (!ff_ws_random(mask, sizeof mask)) {
		snprintf(c->err, sizeof c->err, "no random source for the frame mask");
		return false;
	}
	/* One allocation per send rather than a shared scratch buffer: sends can come from the
	   plugin's own thread while the read loop runs, and a shared buffer would need a lock
	   around something this cheap. */
	size_t cap = len + 16;
	uint8_t *frame = malloc(cap);
	if (!frame) {
		snprintf(c->err, sizeof c->err, "out of memory building a %zu-byte frame", len);
		return false;
	}
	size_t n = ff_ws_build(op, payload, len, mask, frame, cap);
	bool ok = false;
	if (!n) {
		/* The caller gets a reason. Returning false with whatever was in c->err from the
		   last failure reports the wrong cause, which is worse than reporting none. */
		snprintf(c->err, sizeof c->err, "could not build a %d-byte frame", (int)len);
	} else {
		size_t sent = 0;
		ok = true;
		/* A send that cannot make progress has to end. `r == 0` is "would block", and the
		   first version of this loop simply `continue`d on it -- so a stalled TCP window
		   pinned a core at 100% with no timeout and no way out. Bounded, and each empty
		   turn yields rather than spinning: ~200 * 5ms is a second of patience for a frame
		   that is at most a megabyte. */
		int idle = 0;
		while (sent < n) {
			long r = c->io.send(c->io.ctx, frame + sent, n - sent);
			if (r < 0) {
				snprintf(c->err, sizeof c->err, "the connection died while sending");
				ok = false;
				break;
			}
			if (r == 0) {
				if (++idle > 200) {
					snprintf(c->err, sizeof c->err,
						 "the connection stopped accepting data after "
						 "%zu of %zu bytes",
						 sent, n);
					ok = false;
					break;
				}
				os_sleep_ms(5);
				continue;
			}
			idle = 0;
			sent += (size_t)r;
		}
	}
	free(frame);
	return ok;
}

void ff_ws_conn_close(struct ff_ws_conn *c, uint16_t code)
{
	if (!c || c->closing)
		return;
	c->closing = true;
	c->open = false;
	uint8_t body[2] = {(uint8_t)(code >> 8), (uint8_t)code};
	ff_ws_conn_send(c, FF_WS_CLOSE, body, sizeof body);
}

bool ff_ws_conn_init(struct ff_ws_conn *c, struct ff_ws_io io)
{
	if (!c)
		return false;
	memset(c, 0, sizeof *c);
	c->io = io;
	c->rx = malloc(FF_WS_CONN_BUF);
	c->msg = malloc(FF_WS_MAX_PAYLOAD);
	if (!c->rx || !c->msg) {
		ff_ws_conn_free(c);
		return false;
	}
	c->open = true;
	return true;
}

void ff_ws_conn_free(struct ff_ws_conn *c)
{
	if (!c)
		return;
	free(c->rx);
	free(c->msg);
	c->rx = NULL;
	c->msg = NULL;
	c->open = false;
}

static void drop(struct ff_ws_conn *c, size_t used)
{
	memmove(c->rx, c->rx + used, c->rx_len - used);
	c->rx_len -= used;
}

int ff_ws_conn_poll(struct ff_ws_conn *c, struct ff_ws_msg *out)
{
	if (!c || !out || !c->rx || !c->msg || !c->io.recv)
		return -1;

	/* Read once per call, then parse everything already buffered. Parsing the whole buffer
	   matters: a single read can deliver several frames, and a loop that stopped after one
	   would leave the rest sitting until the next byte arrived -- which on a quiet channel is
	   a keepalive interval away. */
	if (c->rx_len < FF_WS_CONN_BUF) {
		long r = c->io.recv(c->io.ctx, c->rx + c->rx_len, FF_WS_CONN_BUF - c->rx_len);
		if (r < 0) {
			snprintf(c->err, sizeof c->err, "the connection died while reading");
			c->open = false;
			return -1;
		}
		c->rx_len += (size_t)r;
	}

	for (;;) {
		struct ff_ws_msg m;
		size_t used = ff_ws_parse(c->rx, c->rx_len, &m);
		if (used == 0)
			return 0; /* incomplete: more bytes will finish it */
		if (used == SIZE_MAX) {
			snprintf(c->err, sizeof c->err, "the server sent a frame we cannot read");
			c->open = false;
			return -1;
		}

		if (m.op == FF_WS_PING) {
			/* Answered here, carrying the ping's own payload back as s5.5.3 requires,
			   and before anything upstream gets a chance to be slow. A pong that waits
			   on the caller's event loop is a pong that arrives after the server has
			   given up on us. */
			if (!ff_ws_conn_send(c, FF_WS_PONG, m.payload, m.len)) {
				/* A server that gets no pong drops us. Reporting the connection as
				   healthy until then means the disconnect arrives later with a
				   different and less useful reason attached to it. */
				c->open = false;
				return -1;
			}
			drop(c, used);
			continue;
		}
		if (m.op == FF_WS_CLOSE) {
			c->close_code = m.len >= 2 ? (uint16_t)((m.payload[0] << 8) | m.payload[1])
						   : 1005;
			snprintf(c->err, sizeof c->err, "the server closed the connection (%u)",
				 c->close_code);
			if (!c->closing) {
				c->closing = true;
				uint8_t echo[2] = {(uint8_t)(c->close_code >> 8),
						   (uint8_t)c->close_code};
				ff_ws_conn_send(c, FF_WS_CLOSE, echo, sizeof echo);
			}
			c->open = false;
			drop(c, used);
			return -1;
		}
		if (m.op == FF_WS_TEXT || m.op == FF_WS_BINARY || m.op == FF_WS_CONT) {
			/* A continuation with nothing to continue, or a new message on top of an
			   unfinished one, both mean we have lost track of the stream. Guessing
			   would splice two messages together and hand up JSON that parses. */
			if (m.op == FF_WS_CONT && !c->in_msg) {
				snprintf(c->err, sizeof c->err,
					 "a continuation frame with no message to continue");
				c->open = false;
				return -1;
			}
			if (m.op != FF_WS_CONT && c->in_msg) {
				snprintf(c->err, sizeof c->err,
					 "a new message started before the previous one finished");
				c->open = false;
				return -1;
			}
			if (m.op != FF_WS_CONT) {
				c->msg_op = m.op;
				c->msg_len = 0;
				c->in_msg = true;
			}
			if (c->msg_len + m.len > FF_WS_MAX_PAYLOAD) {
				snprintf(c->err, sizeof c->err,
					 "a message larger than %u bytes", FF_WS_MAX_PAYLOAD);
				c->open = false;
				return -1;
			}
			if (m.len)
				memcpy(c->msg + c->msg_len, m.payload, m.len);
			c->msg_len += m.len;
			bool done = m.fin;
			drop(c, used);
			if (!done)
				continue;
			c->in_msg = false;
			out->op = c->msg_op;
			out->fin = true;
			out->payload = c->msg;
			out->len = c->msg_len;
			return 1;
		}
		/* PONG, and anything else we do not act on: drop it and keep going */
		drop(c, used);
	}
}
