/* RFC 6455 framing, checked against the RFC's OWN published frames.
 *
 * Why those and not frames this file made up: a mistake here does not produce an error. A client
 * frame must be masked (s5.3); a server that receives an unmasked one "MUST _Fail the WebSocket
 * Connection_" -- it hangs up, with no status text, nothing in a log, and from this side it looks
 * exactly like the network dropping. Testing the builder against its own parser would pass with
 * the masking removed entirely, because both halves would agree. So every frame below is a byte
 * sequence printed in RFC 6455 s5.7 or s1.3, which this code had no hand in choosing.
 *
 * ARMED by mutation -- each guard removed in turn, this file recompiled and rerun. Two of them
 * ("cap check", "server-must-not-mask") leave a variable unused, which -Werror refuses, so those
 * two were disabled with `&& false` rather than deleted; the rest were cut out:
 *
 *     control (every guard in place)           67 checks,  0 failed
 *     masking XOR removed from ff_ws_build     67 checks,  1 failed
 *     cap check removed from ff_ws_build       67 checks, 12 failed
 *     control-frame cap removed from build     67 checks,  2 failed
 *     RSV-bits check removed                   67 checks,  1 failed
 *     reserved-opcode check removed            67 checks,  2 failed
 *     server-must-not-mask check removed       67 checks,  1 failed
 *     control-frame <=125 check removed        67 checks,  1 failed
 *     control-frame FIN check removed          67 checks,  1 failed
 *     minimal 16-bit length form removed       67 checks,  1 failed
 *     minimal 64-bit length form removed       67 checks,  1 failed
 *     FF_WS_MAX_PAYLOAD check removed          67 checks,  1 failed
 *     incomplete-payload check removed         67 checks,  3 failed
 *
 * One of those started out SURVIVING: with the control-frame cap removed, this file stayed green,
 * because the over-long ping was being built into a buffer too small to hold it either way and
 * the cap check answered first. A test that passes for a reason other than the one it is named
 * after is not a test. See `roomy` below.
 *
 * The ordinary frames near the top pass in every one of those mutants, so a parser that simply
 * rejected everything would not get through either.
 */

#include "ff-test.h"
#include <ff-ws.h>
#include <string.h>

/* RFC 6455 s5.7: "A single-frame unmasked text message" containing "Hello". */
static const uint8_t RFC_TEXT_UNMASKED[] = {0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
/* RFC 6455 s5.7: the same message masked, with the mask the RFC uses. */
static const uint8_t RFC_MASK[4] = {0x37, 0xfa, 0x21, 0x3d};
static const uint8_t RFC_TEXT_MASKED[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
/* RFC 6455 s5.7: "A fragmented unmasked text message" -- "Hel" then "lo". */
static const uint8_t RFC_FRAG1[] = {0x01, 0x03, 0x48, 0x65, 0x6c};
static const uint8_t RFC_FRAG2[] = {0x80, 0x02, 0x6c, 0x6f};
/* RFC 6455 s5.7: "Unmasked Ping request" and "masked Pong response", both "Hello". */
static const uint8_t RFC_PING[] = {0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};

static size_t parse(const uint8_t *b, size_t n, struct ff_ws_msg *m)
{
	memset(m, 0, sizeof(*m));
	return ff_ws_parse(b, n, m);
}

int main(void)
{
	struct ff_ws_msg m;
	uint8_t out[128];

	/* ---- the builder produces the RFC's masked bytes, mask and all ---- */
	size_t n = ff_ws_build(FF_WS_TEXT, "Hello", 5, RFC_MASK, out, sizeof(out));
	CHECK(n == sizeof(RFC_TEXT_MASKED));
	CHECK(n == sizeof(RFC_TEXT_MASKED) && memcmp(out, RFC_TEXT_MASKED, n) == 0);

	/* ---- the parser reads the RFC's unmasked frames ---- */
	CHECK(parse(RFC_TEXT_UNMASKED, sizeof(RFC_TEXT_UNMASKED), &m) == sizeof(RFC_TEXT_UNMASKED));
	CHECK(m.op == FF_WS_TEXT);
	CHECK(m.fin);
	CHECK(m.len == 5);
	CHECK(m.payload && memcmp(m.payload, "Hello", 5) == 0);

	CHECK(parse(RFC_FRAG1, sizeof(RFC_FRAG1), &m) == sizeof(RFC_FRAG1));
	CHECK(m.op == FF_WS_TEXT && !m.fin && m.len == 3);
	CHECK(parse(RFC_FRAG2, sizeof(RFC_FRAG2), &m) == sizeof(RFC_FRAG2));
	CHECK(m.op == FF_WS_CONT && m.fin && m.len == 2);

	CHECK(parse(RFC_PING, sizeof(RFC_PING), &m) == sizeof(RFC_PING));
	CHECK(m.op == FF_WS_PING && m.len == 5);

	/* ---- "need more" is not "broken": every truncation point returns 0 ---- */
	CHECK(parse(RFC_TEXT_UNMASKED, 0, &m) == 0);
	CHECK(parse(RFC_TEXT_UNMASKED, 1, &m) == 0);
	CHECK(parse(RFC_TEXT_UNMASKED, 6, &m) == 0); /* header complete, payload one byte short */

	/* a 126-length header is 4 bytes; 3 of them is not yet a length */
	uint8_t big[8 + 300];
	memset(big, 0, sizeof(big));
	big[0] = 0x82; /* FIN + binary */
	big[1] = 126;
	big[2] = 0x01;
	big[3] = 0x00; /* 256 bytes */
	CHECK(parse(big, 3, &m) == 0);
	CHECK(parse(big, 4, &m) == 0);
	CHECK(parse(big, 4 + 256, &m) == 4 + 256);
	CHECK(m.len == 256 && m.op == FF_WS_BINARY);

	/* a 127-length header is 10 bytes */
	uint8_t huge[10 + 8];
	memset(huge, 0, sizeof(huge));
	huge[0] = 0x82;
	huge[1] = 127;
	huge[2 + 5] = 0x01; /* 0x0000000000010000 = 65536 */
	CHECK(parse(huge, 9, &m) == 0);
	CHECK(parse(huge, sizeof(huge), &m) == 0); /* header whole, payload nowhere near */

	/* ---- malformed is SIZE_MAX, and must never be confused with "need more" ---- */
	uint8_t bad[16];

	memcpy(bad, RFC_TEXT_UNMASKED, sizeof(RFC_TEXT_UNMASKED));
	bad[0] |= 0x40; /* RSV1 set, and we negotiate no extensions */
	CHECK(parse(bad, sizeof(RFC_TEXT_UNMASKED), &m) == SIZE_MAX);

	memcpy(bad, RFC_TEXT_UNMASKED, sizeof(RFC_TEXT_UNMASKED));
	bad[0] = 0x83; /* reserved non-control opcode */
	CHECK(parse(bad, sizeof(RFC_TEXT_UNMASKED), &m) == SIZE_MAX);
	bad[0] = 0x8b; /* reserved control opcode */
	CHECK(parse(bad, sizeof(RFC_TEXT_UNMASKED), &m) == SIZE_MAX);

	/* a server MUST NOT mask (s5.1) -- the RFC's own masked CLIENT frame, read as if it had
	   arrived from the server, is exactly that case */
	CHECK(parse(RFC_TEXT_MASKED, sizeof(RFC_TEXT_MASKED), &m) == SIZE_MAX);

	memcpy(bad, RFC_PING, sizeof(RFC_PING));
	bad[0] = 0x09; /* a ping with FIN clear: control frames are never fragmented (s5.5) */
	CHECK(parse(bad, sizeof(RFC_PING), &m) == SIZE_MAX);

	big[0] = 0x89; /* a ping claiming 256 bytes: control payloads are capped at 125 */
	big[1] = 126;
	CHECK(parse(big, 4 + 256, &m) == SIZE_MAX);

	/* the shortest length form that fits must be used */
	big[0] = 0x82;
	big[1] = 126;
	big[2] = 0x00;
	big[3] = 0x64; /* 100, which fits in the 7-bit form */
	CHECK(parse(big, sizeof(big), &m) == SIZE_MAX);

	memset(huge, 0, sizeof(huge));
	huge[0] = 0x82;
	huge[1] = 127;
	huge[2 + 6] = 0x01;
	huge[2 + 7] = 0x00; /* 256, which fits in the 16-bit form */
	CHECK(parse(huge, sizeof(huge), &m) == SIZE_MAX);

	huge[2] = 0x80; /* top bit of the 64-bit length must be 0 (s5.2) */
	CHECK(parse(huge, sizeof(huge), &m) == SIZE_MAX);

	/* a frame claiming more than we will ever buffer is fatal, not "read more" -- a parser
	   that said 0 here would wait for bytes it has already refused to hold */
	memset(huge, 0, sizeof(huge));
	huge[0] = 0x82;
	huge[1] = 127;
	uint64_t over = (uint64_t)FF_WS_MAX_PAYLOAD + 1;
	for (int s = 0; s < 8; s++)
		huge[2 + s] = (uint8_t)(over >> (56 - 8 * s));
	CHECK(parse(huge, sizeof(huge), &m) == SIZE_MAX);

	CHECK(ff_ws_parse(NULL, 4, &m) == SIZE_MAX);
	CHECK(ff_ws_parse(RFC_PING, sizeof(RFC_PING), NULL) == SIZE_MAX);

	/* ---- the builder refuses rather than writing a partial frame ---- */
	uint8_t untouched[16];
	memset(untouched, 0xab, sizeof(untouched));
	CHECK(ff_ws_build(FF_WS_TEXT, "Hello", 5, RFC_MASK, untouched, 10) == 0);
	for (size_t i = 0; i < sizeof(untouched); i++)
		CHECK(untouched[i] == 0xab);

	/* A control frame's payload is capped at 125 bytes (s5.5). The buffer here is deliberately
	   ROOMY: the first version passed this in a 128-byte `out`, where the frame would not have
	   fit anyway, so the cap check was answering and the control-frame rule was never exercised
	   -- removing that rule from ff_ws_build left this file entirely green. Found by mutating. */
	static uint8_t roomy[512];
	char big_payload[200];
	memset(big_payload, 'x', sizeof(big_payload));
	CHECK(ff_ws_build(FF_WS_PING, big_payload, sizeof(big_payload), RFC_MASK, roomy, sizeof(roomy)) == 0);
	CHECK(ff_ws_build(FF_WS_CLOSE, big_payload, sizeof(big_payload), RFC_MASK, roomy, sizeof(roomy)) == 0);
	/* exactly at the limit is legal, so the guard is a cap and not a blanket refusal */
	CHECK(ff_ws_build(FF_WS_PING, big_payload, 125, RFC_MASK, roomy, sizeof(roomy)) == 6 + 125);
	CHECK(ff_ws_build(FF_WS_TEXT, "Hello", 5, RFC_MASK, NULL, 64) == 0);
	CHECK(ff_ws_build(FF_WS_TEXT, "Hello", 5, NULL, out, sizeof(out)) == 0);
	CHECK(ff_ws_build(FF_WS_TEXT, NULL, 5, RFC_MASK, out, sizeof(out)) == 0);
	/* an empty payload is legal and is not "unusable arguments" */
	CHECK(ff_ws_build(FF_WS_CLOSE, NULL, 0, RFC_MASK, out, sizeof(out)) == 6);

	/* header grows at exactly the documented boundaries: 2+4, then +2, then +8 */
	static uint8_t payload[70000];
	uint8_t frame[70016];
	CHECK(ff_ws_build(FF_WS_BINARY, payload, 125, RFC_MASK, frame, sizeof(frame)) == 6 + 125);
	CHECK(ff_ws_build(FF_WS_BINARY, payload, 126, RFC_MASK, frame, sizeof(frame)) == 8 + 126);
	CHECK(ff_ws_build(FF_WS_BINARY, payload, 65535, RFC_MASK, frame, sizeof(frame)) == 8 + 65535);
	CHECK(ff_ws_build(FF_WS_BINARY, payload, 65536, RFC_MASK, frame, sizeof(frame)) == 14 + 65536);

	/* ---- Sec-WebSocket-Accept, from the example in RFC 6455 s1.3 ---- */
	char acc[64];
	CHECK(ff_ws_accept_for("dGhlIHNhbXBsZSBub25jZQ==", acc, sizeof(acc)));
	CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
	CHECK(!ff_ws_accept_for(NULL, acc, sizeof(acc)));
	CHECK(!ff_ws_accept_for("dGhlIHNhbXBsZSBub25jZQ==", acc, 8));
	CHECK(!ff_ws_accept_for("dGhlIHNhbXBsZSBub25jZQ==", NULL, sizeof(acc)));

	FF_TEST_MAIN_END();
}
