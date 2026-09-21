/* The WebSocket connection: the upgrade, and the read loop.
 *
 * None of this touches a network. The socket is two function pointers, so every case that a live
 * connection would only produce by luck is scripted here instead: a frame split across two reads,
 * two frames in one read, the 101 response and the first frame in the SAME read, a ping that has
 * to be answered from inside the loop, a fragmented message, and a plain HTTP 200 from something
 * that is not a WebSocket server at all.
 *
 * That last one is the reason the handshake is checked at all. A captive portal or an intercepting
 * proxy answers with a perfectly valid HTTP response; a client that only asks "did the server say
 * something" then reads HTML as frames and reports a protocol error from somewhere far away.
 *
 * ARMED by mutation -- each guard removed in turn, recompiled and rerun. Where removing a guard
 * outright leaves a variable unused (-Werror refuses that), it was disabled with `&& false`:
 *
 *     control (every guard in place)               87 checks,  0 failed
 *     101 status check removed                     87 checks,  1 failed
 *     accept-value comparison removed              87 checks,  2 failed
 *     Upgrade header check removed                 87 checks,  1 failed
 *     returns whole length, not header length      87 checks,  1 failed
 *     header match made case-sensitive             87 checks,  1 failed
 *     runaway-header guard removed                 87 checks,  1 failed
 *     ping no longer answered                      87 checks,  4 failed
 *     close no longer echoed                       87 checks,  2 failed
 *     loop stops at the first control frame        87 checks,  2 failed
 *     orphan continuation allowed                  87 checks,  2 failed
 *     interleaved message allowed                  87 checks,  2 failed
 *     consumed frame not dropped after emit        87 checks,  5 failed
 *     frame mask forced to zero                    87 checks,  1 failed
 *
 * One mutation was a dud at first -- a "stops after one frame" edit that changed no behaviour and
 * scored 0 failed, which reads exactly like a missing test. The loop only matters when a control
 * frame SHARES a read with a message, so that case was added and the real mutation then bit.
 */

#include "ff-test.h"
#include <ff-ws-conn.h>
#include <string.h>

/* The RFC 6455 s1.3 example key, so the request and the expected accept value are both the
   document's own bytes rather than ones this file chose. */
#define KEY "dGhlIHNhbXBsZSBub25jZQ=="
#define ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

/* ---- a scripted socket ---------------------------------------------------------------------
   `chunks` is how the bytes are handed over: recv returns at most one chunk per call, which is
   what makes "split across two reads" a thing this file can actually cause. */
struct script {
	const uint8_t *data;
	size_t len;
	size_t pos;
	size_t chunk;   /* max bytes per recv; 0 means "everything available" */
	bool dead;      /* recv reports a dead socket */
	uint8_t sent[4096];
	size_t sent_len;
};

static long s_recv(void *ctx, void *buf, size_t len)
{
	struct script *s = ctx;
	if (s->dead)
		return -1;
	size_t avail = s->len - s->pos;
	if (!avail)
		return 0;
	size_t n = avail;
	if (s->chunk && n > s->chunk)
		n = s->chunk;
	if (n > len)
		n = len;
	memcpy(buf, s->data + s->pos, n);
	s->pos += n;
	return (long)n;
}

static long s_send(void *ctx, const void *buf, size_t len)
{
	struct script *s = ctx;
	if (s->sent_len + len > sizeof s->sent)
		return -1;
	memcpy(s->sent + s->sent_len, buf, len);
	s->sent_len += len;
	return (long)len;
}

/* Decodes one masked CLIENT frame out of the capture buffer, so what was actually put on the
   wire can be inspected -- including that it was masked at all. */
static bool decode_sent(const uint8_t *f, size_t len, uint8_t *op, uint8_t *payload, size_t *plen)
{
	if (len < 6)
		return false;
	*op = f[0] & 0x0f;
	if (!(f[1] & 0x80))
		return false; /* not masked: a client frame must be */
	size_t n = f[1] & 0x7f;
	if (n > 125 || len < 6 + n)
		return false;
	for (size_t i = 0; i < n; i++)
		payload[i] = f[6 + i] ^ f[2 + (i & 3)];
	*plen = n;
	return true;
}

static struct ff_ws_conn *fresh(struct script *s)
{
	static struct ff_ws_conn c;
	struct ff_ws_io io = {.send = s_send, .recv = s_recv, .ctx = s};
	ff_ws_conn_free(&c);
	CHECK(ff_ws_conn_init(&c, io));
	return &c;
}

static const char OK101[] =
	"HTTP/1.1 101 Switching Protocols\r\n"
	"Upgrade: websocket\r\n"
	"Connection: Upgrade\r\n"
	"Sec-WebSocket-Accept: " ACCEPT "\r\n"
	"\r\n";

int main(void)
{
	char err[256];
	char req[512];

	/* ---- the upgrade request ---- */
	size_t n = ff_ws_handshake_request("eventsub.wss.twitch.tv", "/ws", KEY, req, sizeof req);
	CHECK(n > 0);
	CHECK(strstr(req, "GET /ws HTTP/1.1\r\n") == req);
	CHECK(strstr(req, "Host: eventsub.wss.twitch.tv\r\n") != NULL);
	CHECK(strstr(req, "Upgrade: websocket\r\n") != NULL);
	CHECK(strstr(req, "Connection: Upgrade\r\n") != NULL);
	CHECK(strstr(req, "Sec-WebSocket-Key: " KEY "\r\n") != NULL);
	CHECK(strstr(req, "Sec-WebSocket-Version: 13\r\n") != NULL);
	CHECK(n >= 4 && memcmp(req + n - 4, "\r\n\r\n", 4) == 0);
	/* a buffer that cannot hold it gets nothing, not a truncated request */
	CHECK(ff_ws_handshake_request("eventsub.wss.twitch.tv", "/ws", KEY, req, 40) == 0);

	/* ---- the response ---- */
	err[0] = 0;
	CHECK(ff_ws_handshake_check(OK101, strlen(OK101), KEY, err, sizeof err) == strlen(OK101));
	CHECK(err[0] == 0);

	/* the 101 and the first frame in ONE read: the return value is the HEADERS' length, so the
	   caller keeps the frame. A reader that discarded its buffer here would lose a message and
	   report nothing at all. */
	char both[512];
	size_t hlen = strlen(OK101);
	memcpy(both, OK101, hlen);
	const uint8_t first[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
	memcpy(both + hlen, first, sizeof first);
	CHECK(ff_ws_handshake_check(both, hlen + sizeof first, KEY, err, sizeof err) == hlen);

	/* not complete yet -- and "not complete" must not be "not a WebSocket server" */
	CHECK(ff_ws_handshake_check(OK101, 20, KEY, err, sizeof err) == 0);
	CHECK(ff_ws_handshake_check("HTTP/1.1 101 Switching Protocols\r\nUpgrade: web", 46, KEY,
				    err, sizeof err) == 0);

	/* a proxy or captive portal answering instead of the server */
	static const char PAGE[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html>";
	err[0] = 0;
	CHECK(ff_ws_handshake_check(PAGE, strlen(PAGE), KEY, err, sizeof err) == SIZE_MAX);
	CHECK(strstr(err, "200") != NULL);

	/* 101, but the accept value does not answer OUR key */
	static const char WRONG[] =
		"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\nSec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n";
	err[0] = 0;
	CHECK(ff_ws_handshake_check(WRONG, strlen(WRONG), KEY, err, sizeof err) == SIZE_MAX);
	CHECK(strstr(err, "does not match") != NULL);

	static const char NOUP[] =
		"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: " ACCEPT "\r\n\r\n";
	CHECK(ff_ws_handshake_check(NOUP, strlen(NOUP), KEY, err, sizeof err) == SIZE_MAX);

	static const char NOACC[] =
		"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
		"Connection: Upgrade\r\n\r\n";
	CHECK(ff_ws_handshake_check(NOACC, strlen(NOACC), KEY, err, sizeof err) == SIZE_MAX);

	/* header names are case-insensitive (RFC 7230 s3.2) and servers do vary -- refusing a
	   lowercase 'upgrade' would fail against a perfectly correct server */
	static const char LOWER[] =
		"HTTP/1.1 101 Switching Protocols\r\nupgrade: WebSocket\r\n"
		"connection: upgrade\r\nsec-websocket-accept: " ACCEPT "\r\n\r\n";
	CHECK(ff_ws_handshake_check(LOWER, strlen(LOWER), KEY, err, sizeof err) == strlen(LOWER));

	/* something that will never send a blank line must not hold the connection open forever */
	static char FLOOD[9000];
	memset(FLOOD, 'x', sizeof FLOOD);
	memcpy(FLOOD, "HTTP/1.1 101 x\r\n", 16);
	CHECK(ff_ws_handshake_check(FLOOD, sizeof FLOOD, KEY, err, sizeof err) == SIZE_MAX);

	/* ---- the read loop ---- */
	struct ff_ws_msg m;

	/* one whole frame */
	static const uint8_t HELLO[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
	struct script s1 = {.data = HELLO, .len = sizeof HELLO};
	struct ff_ws_conn *c = fresh(&s1);
	CHECK(ff_ws_conn_poll(c, &m) == 1);
	CHECK(m.op == FF_WS_TEXT && m.len == 5 && memcmp(m.payload, "Hello", 5) == 0);
	CHECK(ff_ws_conn_poll(c, &m) == 0); /* nothing more, and not an error */

	/* the same frame delivered one byte at a time */
	struct script s2 = {.data = HELLO, .len = sizeof HELLO, .chunk = 1};
	c = fresh(&s2);
	int got = 0;
	for (int i = 0; i < 10; i++) {
		int r = ff_ws_conn_poll(c, &m);
		CHECK(r >= 0);
		if (r == 1)
			got++;
	}
	CHECK(got == 1);
	CHECK(m.len == 5 && memcmp(m.payload, "Hello", 5) == 0);

	/* two frames arriving in ONE read: the second must not wait for more bytes */
	static const uint8_t TWO[] = {0x81, 0x02, 'h', 'i', 0x81, 0x03, 'y', 'o', 'u'};
	struct script s3 = {.data = TWO, .len = sizeof TWO};
	c = fresh(&s3);
	CHECK(ff_ws_conn_poll(c, &m) == 1);
	CHECK(m.len == 2 && memcmp(m.payload, "hi", 2) == 0);
	s3.dead = false;
	CHECK(ff_ws_conn_poll(c, &m) == 1);
	CHECK(m.len == 3 && memcmp(m.payload, "you", 3) == 0);

	/* a ping is answered from inside the loop, with its own payload, masked */
	static const uint8_t PING[] = {0x89, 0x04, 'p', 'i', 'n', 'g'};
	struct script s4 = {.data = PING, .len = sizeof PING};
	c = fresh(&s4);
	CHECK(ff_ws_conn_poll(c, &m) == 0); /* a ping is not an application message */
	uint8_t op = 0, pl[128];
	size_t pn = 0;
	CHECK(s4.sent_len > 0);
	CHECK(decode_sent(s4.sent, s4.sent_len, &op, pl, &pn));
	CHECK(op == FF_WS_PONG);
	CHECK(pn == 4 && memcmp(pl, "ping", 4) == 0);

	/* a ping and a message in ONE read: the message must not wait for the next poll. A loop
	   that stopped at the control frame would hold every notification that happened to share a
	   packet with a keepalive ping until the next byte arrived. */
	static const uint8_t PING_THEN_MSG[] = {0x89, 0x01, 'p', 0x81, 0x02, 'o', 'k'};
	struct script s4b = {.data = PING_THEN_MSG, .len = sizeof PING_THEN_MSG};
	c = fresh(&s4b);
	CHECK(ff_ws_conn_poll(c, &m) == 1);
	CHECK(m.len == 2 && memcmp(m.payload, "ok", 2) == 0);

	/* a close ends the connection, reports the code, and is echoed back */
	static const uint8_t CLOSE[] = {0x88, 0x02, 0x03, 0xe8}; /* 1000 normal */
	struct script s5 = {.data = CLOSE, .len = sizeof CLOSE};
	c = fresh(&s5);
	CHECK(ff_ws_conn_poll(c, &m) == -1);
	CHECK(c->close_code == 1000);
	CHECK(!c->open);
	CHECK(decode_sent(s5.sent, s5.sent_len, &op, pl, &pn));
	CHECK(op == FF_WS_CLOSE);

	/* a message split over two frames arrives as ONE message */
	static const uint8_t FRAG[] = {0x01, 0x03, 'H', 'e', 'l', 0x80, 0x02, 'l', 'o'};
	struct script s6 = {.data = FRAG, .len = sizeof FRAG};
	c = fresh(&s6);
	CHECK(ff_ws_conn_poll(c, &m) == 1);
	CHECK(m.len == 5 && memcmp(m.payload, "Hello", 5) == 0);
	CHECK(m.op == FF_WS_TEXT); /* the FIRST frame's opcode, not the continuation's */

	/* a continuation with nothing to continue is a lost stream, not something to guess at */
	static const uint8_t ORPHAN[] = {0x80, 0x02, 'l', 'o'};
	struct script s7 = {.data = ORPHAN, .len = sizeof ORPHAN};
	c = fresh(&s7);
	CHECK(ff_ws_conn_poll(c, &m) == -1);
	CHECK(strstr(c->err, "continuation") != NULL);

	/* a new message on top of an unfinished one: splicing them would hand up JSON that parses */
	static const uint8_t INTERLEAVED[] = {0x01, 0x03, 'H', 'e', 'l', 0x81, 0x02, 'h', 'i'};
	struct script s8 = {.data = INTERLEAVED, .len = sizeof INTERLEAVED};
	c = fresh(&s8);
	CHECK(ff_ws_conn_poll(c, &m) == -1);
	CHECK(strstr(c->err, "before the previous one finished") != NULL);

	/* a dead socket is reported, not spun on */
	struct script s9 = {.data = HELLO, .len = sizeof HELLO, .dead = true};
	c = fresh(&s9);
	CHECK(ff_ws_conn_poll(c, &m) == -1);
	CHECK(!c->open);

	/* a frame the parser refuses ends the connection rather than being skipped */
	static const uint8_t RSV[] = {0xc1, 0x02, 'h', 'i'};
	struct script s10 = {.data = RSV, .len = sizeof RSV};
	c = fresh(&s10);
	CHECK(ff_ws_conn_poll(c, &m) == -1);

	/* what we SEND is masked, as a client must be -- the RFC's requirement, and the one whose
	   failure mode is the server hanging up with no error text */
	struct script s11 = {.data = HELLO, .len = 0};
	c = fresh(&s11);
	CHECK(ff_ws_conn_send(c, FF_WS_TEXT, "hey", 3));
	CHECK(s11.sent_len == 9);
	CHECK((s11.sent[1] & 0x80) != 0);
	CHECK(decode_sent(s11.sent, s11.sent_len, &op, pl, &pn));
	CHECK(op == FF_WS_TEXT && pn == 3 && memcmp(pl, "hey", 3) == 0);
	/* and the mask is not a constant: two sends of the same text must differ on the wire */
	size_t firstlen = s11.sent_len;
	CHECK(ff_ws_conn_send(c, FF_WS_TEXT, "hey", 3));
	CHECK(memcmp(s11.sent, s11.sent + firstlen, firstlen) != 0);

	/* ---- the random source ---- */
	uint8_t r1[16], r2[16];
	CHECK(ff_ws_random(r1, sizeof r1));
	CHECK(ff_ws_random(r2, sizeof r2));
	CHECK(memcmp(r1, r2, sizeof r1) != 0);
	CHECK(!ff_ws_random(NULL, 16));
	CHECK(!ff_ws_random(r1, 0));

	ff_ws_conn_free(c);
	FF_TEST_MAIN_END();
}
