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
 *     control (every guard in place)              101 checks,  0 failed
 *     101 status check removed                    101 checks,  1 failed
 *     accept-value comparison removed             101 checks,  2 failed
 *     Upgrade header check removed                101 checks,  1 failed
 *     Connection header check removed             101 checks,  1 failed
 *     Connection list rejected (exact match only) 101 checks,  1 failed
 *     returns whole length, not header length     101 checks,  1 failed
 *     runaway-header guard removed                101 checks,  1 failed
 *     ping no longer answered                     101 checks,  4 failed
 *     loop stops at the first control frame       101 checks,  2 failed
 *     orphan continuation allowed                 101 checks,  2 failed
 *     interleaved message allowed                 101 checks,  2 failed
 *     reassembly bounds check removed             101 checks,  2 failed
 *     consumed frame not dropped after emit       101 checks,  6 failed
 *     frame mask forced to zero                   101 checks,  1 failed
 *     would-block send spins again (no cap)       101 checks,  2 failed
 *
 * Three of these started out surviving, at a clean score, and each one was a missing case rather
 * than a pointless guard:
 *   - a "stops after one frame" edit that changed no behaviour. The loop only matters when a
 *     control frame SHARES a read with a message, so that case was added.
 *   - the reassembly bounds check -- the only memory-safety guard in this file, and the only one
 *     with no test. Two full-size fragments write a megabyte past the buffer.
 *   - the Connection header. Upgrade-missing and Accept-missing were both tested; the third of
 *     the trio was not.
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
	size_t chunk; /* max bytes per recv; 0 means "everything available" */
	bool dead;    /* recv reports a dead socket */
	uint8_t sent[4096];
	size_t sent_len;
	const long *send_script; /* per-call: 0 = would block, n = accept at most n bytes */
	size_t send_len, send_pos;
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

/* `send_script` makes the socket behave like a real one under load: 0 means "would block" and a
   smaller number is a short write. A scripted send that always accepts everything never produces
   either, which is how the send loop's would-block path went untested while it busy-spun. */
static long s_send(void *ctx, const void *buf, size_t len)
{
	struct script *s = ctx;
	if (s->send_script && s->send_pos < s->send_len) {
		long allow = s->send_script[s->send_pos++];
		if (allow == 0)
			return 0; /* would block */
		if ((size_t)allow < len)
			len = (size_t)allow;
	}
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

static const char OK101[] = "HTTP/1.1 101 Switching Protocols\r\n"
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
	CHECK(ff_ws_handshake_check("HTTP/1.1 101 Switching Protocols\r\nUpgrade: web", 46, KEY, err, sizeof err) == 0);

	/* a proxy or captive portal answering instead of the server */
	static const char PAGE[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html>";
	err[0] = 0;
	CHECK(ff_ws_handshake_check(PAGE, strlen(PAGE), KEY, err, sizeof err) == SIZE_MAX);
	CHECK(strstr(err, "200") != NULL);

	/* 101, but the accept value does not answer OUR key */
	static const char WRONG[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
				    "Connection: Upgrade\r\nSec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n";
	err[0] = 0;
	CHECK(ff_ws_handshake_check(WRONG, strlen(WRONG), KEY, err, sizeof err) == SIZE_MAX);
	CHECK(strstr(err, "does not match") != NULL);

	static const char NOUP[] = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
				   "Sec-WebSocket-Accept: " ACCEPT "\r\n\r\n";
	CHECK(ff_ws_handshake_check(NOUP, strlen(NOUP), KEY, err, sizeof err) == SIZE_MAX);

	/* The missing half of the trio: Upgrade-missing and Accept-missing were both tested and
	   Connection-missing was not, so removing that check scored a clean 98/98. */
	static const char NOCONN[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
				     "Sec-WebSocket-Accept: " ACCEPT "\r\n\r\n";
	CHECK(ff_ws_handshake_check(NOCONN, strlen(NOCONN), KEY, err, sizeof err) == SIZE_MAX);

	/* `Connection` is a comma-separated LIST (RFC 7230 s6.1), and a proxy in the path really
	   does send "keep-alive, Upgrade". Demanding the whole value be "Upgrade" refuses a
	   correct server, which costs more than letting a wrong one through -- the accept value is
	   what actually proves this is a WebSocket server. */
	static const char LIST[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
				   "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: " ACCEPT "\r\n\r\n";
	CHECK(ff_ws_handshake_check(LIST, strlen(LIST), KEY, err, sizeof err) == strlen(LIST));

	/* something that is not HTTP at all */
	static const char NOTHTTP[] = "\x16\x03\x01 this is not a response\r\n\r\n";
	CHECK(ff_ws_handshake_check(NOTHTTP, strlen(NOTHTTP), KEY, err, sizeof err) == SIZE_MAX);

	static const char NOACC[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
				    "Connection: Upgrade\r\n\r\n";
	CHECK(ff_ws_handshake_check(NOACC, strlen(NOACC), KEY, err, sizeof err) == SIZE_MAX);

	/* header names are case-insensitive (RFC 7230 s3.2) and servers do vary -- refusing a
	   lowercase 'upgrade' would fail against a perfectly correct server */
	static const char LOWER[] = "HTTP/1.1 101 Switching Protocols\r\nupgrade: WebSocket\r\n"
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

	/* A message reassembled past the buffer it is being written into. This is the only
	   memory-safety guard in ff-ws-conn.c, and it was the only one missing from the table
	   below -- disabling it left this file entirely green. The write target is 1 MiB, and two
	   1 MiB fragments would run a megabyte past it; the writer is the server, so this needs a
	   hostile endpoint past TLS, which is exactly the case a bounds check is for. */
	{
		static uint8_t huge[2 * (FF_WS_MAX_PAYLOAD + 16)];
		size_t n = 0;
		/* frame 1: TEXT, FIN clear, a full FF_WS_MAX_PAYLOAD payload (64-bit length) */
		huge[n++] = 0x01;
		huge[n++] = 127;
		for (int sh = 56; sh >= 0; sh -= 8)
			huge[n++] = (uint8_t)((uint64_t)FF_WS_MAX_PAYLOAD >> sh);
		memset(huge + n, 'a', FF_WS_MAX_PAYLOAD);
		n += FF_WS_MAX_PAYLOAD;
		/* frame 2: one more byte, FIN set -- one byte too many */
		huge[n++] = 0x80;
		huge[n++] = 0x01;
		huge[n++] = 'b';

		struct script sh = {.data = huge, .len = n};
		c = fresh(&sh);
		int r;
		int guard = 0;
		/* the first frame needs several reads to arrive */
		while ((r = ff_ws_conn_poll(c, &m)) == 0 && ++guard < 64)
			;
		CHECK(r == -1);
		CHECK(strstr(c->err, "larger than") != NULL);
		if (r != -1)
			fprintf(stderr, "      (a message past the buffer was accepted)\n");
	}

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

	/* A socket that blocks, then dribbles the frame out a few bytes at a time. Both are
	   ordinary on a slow uplink while OBS is also pushing video, and the send loop used to
	   answer a would-block by immediately trying again -- an uninterruptible full-core spin
	   inside the worker thread, which also meant shutdown could never join it. */
	static const long DRIBBLE[] = {0, 0, 2, 1, 0, 3, 100};
	struct script s12 = {.data = HELLO,
			     .len = 0,
			     .send_script = DRIBBLE,
			     .send_len = sizeof DRIBBLE / sizeof DRIBBLE[0]};
	c = fresh(&s12);
	CHECK(ff_ws_conn_send(c, FF_WS_TEXT, "hey", 3));
	CHECK(s12.sent_len == 9); /* the whole frame still arrives, in pieces */
	CHECK(decode_sent(s12.sent, s12.sent_len, &op, pl, &pn));
	CHECK(op == FF_WS_TEXT && pn == 3 && memcmp(pl, "hey", 3) == 0);

	/* A socket that NEVER accepts anything has to be given up on, not waited on forever. */
	static const long NEVER[1024] = {0};
	struct script s13 = {.data = HELLO, .len = 0, .send_script = NEVER, .send_len = sizeof NEVER / sizeof NEVER[0]};
	c = fresh(&s13);
	CHECK(!ff_ws_conn_send(c, FF_WS_TEXT, "hey", 3));
	CHECK(strstr(c->err, "stopped accepting") != NULL);

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
