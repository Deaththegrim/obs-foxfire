#include "ff-ws.h"
#include <string.h>

/* ------------------------------------------------------------------ framing */

size_t ff_ws_build(enum ff_ws_opcode op, const void *payload, size_t len, const uint8_t mask[4],
		   uint8_t *out, size_t cap)
{
	if (!out || !mask || (len && !payload))
		return 0;
	/* A control frame's payload is capped at 125 bytes and it may not be fragmented
	   (RFC 6455 s5.5). Refusing here beats emitting a frame a server will close on. */
	if ((op & 0x8) && len > 125)
		return 0;

	size_t header = 2 + 4; /* fin/op, len byte, and always a 4-byte mask: clients must mask */
	if (len > 65535)
		header += 8;
	else if (len > 125)
		header += 2;
	if (cap < header + len)
		return 0;

	size_t i = 0;
	out[i++] = (uint8_t)(0x80 | (uint8_t)op); /* FIN: we never fragment what we send */
	if (len > 65535) {
		out[i++] = 0x80 | 127;
		for (int s = 56; s >= 0; s -= 8)
			out[i++] = (uint8_t)((uint64_t)len >> s);
	} else if (len > 125) {
		out[i++] = 0x80 | 126;
		out[i++] = (uint8_t)(len >> 8);
		out[i++] = (uint8_t)len;
	} else {
		out[i++] = (uint8_t)(0x80 | len);
	}
	memcpy(out + i, mask, 4);
	i += 4;
	const uint8_t *p = payload;
	for (size_t k = 0; k < len; k++)
		out[i + k] = (uint8_t)(p[k] ^ mask[k & 3]);
	return i + len;
}

size_t ff_ws_parse(const uint8_t *buf, size_t len, struct ff_ws_msg *out)
{
	if (!buf || !out)
		return SIZE_MAX;
	if (len < 2)
		return 0;

	bool fin = (buf[0] & 0x80) != 0;
	/* RSV1..3 must be zero unless an extension was negotiated, and we negotiate none. A set
	   bit means we are reading something we do not understand, which is not recoverable by
	   reading further. */
	if (buf[0] & 0x70)
		return SIZE_MAX;
	uint8_t op = buf[0] & 0x0f;
	switch (op) {
	case FF_WS_CONT:
	case FF_WS_TEXT:
	case FF_WS_BINARY:
	case FF_WS_CLOSE:
	case FF_WS_PING:
	case FF_WS_PONG:
		break;
	default:
		return SIZE_MAX; /* a reserved opcode; same reasoning as RSV */
	}
	bool masked = (buf[1] & 0x80) != 0;
	/* A server MUST NOT mask (RFC 6455 s5.1). Accepting one anyway would mean quietly talking
	   to something that is not following the protocol. */
	if (masked)
		return SIZE_MAX;

	uint64_t plen = buf[1] & 0x7f;
	size_t i = 2;
	if (plen == 126) {
		if (len < 4)
			return 0;
		plen = ((uint64_t)buf[2] << 8) | buf[3];
		i = 4;
		/* the shortest form that could have carried this length is the one that must be
		   used; a 126 frame carrying 100 bytes is malformed */
		if (plen < 126)
			return SIZE_MAX;
	} else if (plen == 127) {
		if (len < 10)
			return 0;
		plen = 0;
		for (int s = 0; s < 8; s++)
			plen = (plen << 8) | buf[2 + s];
		i = 10;
		if (plen < 65536)
			return SIZE_MAX;
		/* the top bit must be 0 (s5.2), and anything that large is hostile anyway */
		if (plen & 0x8000000000000000ull)
			return SIZE_MAX;
	}
	if ((op & 0x8) && (plen > 125 || !fin))
		return SIZE_MAX; /* control frames: <=125 bytes, never fragmented */
	if (plen > FF_WS_MAX_PAYLOAD)
		return SIZE_MAX;
	if (len < i + (size_t)plen)
		return 0; /* incomplete: read more and ask again */

	out->op = (enum ff_ws_opcode)op;
	out->fin = fin;
	out->payload = plen ? buf + i : NULL;
	out->len = (size_t)plen;
	return i + (size_t)plen;
}

/* -------------------------------------------------------------- handshake bits */

/* SHA-1, RFC 3174. Here only because the WebSocket handshake specifies it; it is not used for
   anything security-bearing and must not be (the licence signatures are Ed25519, in ff-licence.c).
   Verifying Sec-WebSocket-Accept is what stops a plain HTTP endpoint, or a proxy that answered on
   the server's behalf, being mistaken for a WebSocket server. */
struct sha1 {
	uint32_t h[5];
	uint64_t bits;
	uint8_t buf[64];
	size_t n;
};

static uint32_t rol(uint32_t v, int s)
{
	return (v << s) | (v >> (32 - s));
}

static void sha1_block(struct sha1 *c, const uint8_t *p)
{
	uint32_t w[80];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	for (int i = 16; i < 80; i++)
		w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
	for (int i = 0; i < 80; i++) {
		uint32_t k, t;
		if (i < 20) {
			t = (b & d) | (~b & e);
			k = 0x5a827999;
		} else if (i < 40) {
			t = b ^ d ^ e;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			t = (b & d) | (b & e) | (d & e);
			k = 0x8f1bbcdc;
		} else {
			t = b ^ d ^ e;
			k = 0xca62c1d6;
		}
		uint32_t tmp = rol(a, 5) + t + f + k + w[i];
		f = e;
		e = d;
		d = rol(b, 30);
		b = a;
		a = tmp;
	}
	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += d;
	c->h[3] += e;
	c->h[4] += f;
}

static void sha1_init(struct sha1 *c)
{
	c->h[0] = 0x67452301;
	c->h[1] = 0xefcdab89;
	c->h[2] = 0x98badcfe;
	c->h[3] = 0x10325476;
	c->h[4] = 0xc3d2e1f0;
	c->bits = 0;
	c->n = 0;
}

static void sha1_update(struct sha1 *c, const uint8_t *p, size_t n)
{
	c->bits += (uint64_t)n * 8;
	while (n) {
		size_t take = 64 - c->n;
		if (take > n)
			take = n;
		memcpy(c->buf + c->n, p, take);
		c->n += take;
		p += take;
		n -= take;
		if (c->n == 64) {
			sha1_block(c, c->buf);
			c->n = 0;
		}
	}
}

static void sha1_final(struct sha1 *c, uint8_t out[20])
{
	uint64_t bits = c->bits;
	uint8_t pad = 0x80;
	sha1_update(c, &pad, 1);
	uint8_t z = 0;
	while (c->n != 56)
		sha1_update(c, &z, 1);
	uint8_t len[8];
	for (int i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - i * 8));
	/* sha1_update would add these to the count; write the block directly instead */
	memcpy(c->buf + 56, len, 8);
	sha1_block(c, c->buf);
	for (int i = 0; i < 5; i++) {
		out[i * 4] = (uint8_t)(c->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)c->h[i];
	}
}

static size_t b64(const uint8_t *in, size_t n, char *out, size_t cap)
{
	static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t need = ((n + 2) / 3) * 4;
	if (cap < need + 1)
		return 0;
	size_t j = 0;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < n)
			v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < n)
			v |= in[i + 2];
		out[j++] = A[(v >> 18) & 63];
		out[j++] = A[(v >> 12) & 63];
		out[j++] = (i + 1 < n) ? A[(v >> 6) & 63] : '=';
		out[j++] = (i + 2 < n) ? A[v & 63] : '=';
	}
	out[j] = '\0';
	return j;
}

bool ff_ws_accept_for(const char *key_b64, char *out, size_t cap)
{
	static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"; /* RFC 6455 s1.3 */
	if (!key_b64 || !out || cap < 29)
		return false;
	struct sha1 c;
	sha1_init(&c);
	sha1_update(&c, (const uint8_t *)key_b64, strlen(key_b64));
	sha1_update(&c, (const uint8_t *)GUID, sizeof GUID - 1);
	uint8_t digest[20];
	sha1_final(&c, digest);
	return b64(digest, sizeof digest, out, cap) != 0;
}
