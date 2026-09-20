#include "ff-test.h"
#include "ff-licence.h"
#include "monocypher-ed25519.h"
#include <string.h>
#include <stdio.h>

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const uint8_t *in, size_t n, char *out)
{
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
			     (i + 2 < n ? in[i + 2] : 0);
		out[o++] = B64[v >> 18 & 63];
		out[o++] = B64[v >> 12 & 63];
		out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
		out[o++] = i + 2 < n ? B64[v & 63] : '=';
	}
	out[o] = 0;
}

static void make(const uint8_t sk[64], const char *pack, int64_t issued, int64_t expires, char *json, size_t cap,
		 int corrupt)
{
	char canon[512];
	size_t n = ff_licence_canonical("123456789012345678", pack, "lic_test1", issued, expires, canon, sizeof canon);
	uint8_t sig[64];
	crypto_ed25519_sign(sig, sk, (const uint8_t *)canon, n);
	if (corrupt)
		sig[10] ^= 0x40;
	char s64[100];
	b64(sig, 64, s64);
	snprintf(
		json, cap,
		"{\"discord_id\":\"123456789012345678\",\"expires\":%lld,\"issued\":%lld,\"licence_id\":\"lic_test1\",\"pack_id\":\"%s\",\"sig\":\"%s\"}",
		(long long)expires, (long long)issued, pack, s64);
}

int main(void)
{
	uint8_t seed[32] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
			    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
	uint8_t sk[64], pk[32];
	crypto_ed25519_key_pair(sk, pk, seed);
	char json[1024];
	struct ff_licence L;
	const int64_t issued = 1758240000, expires = issued + 90 * 86400;

	make(sk, "ember", issued, expires, json, sizeof json, 0);
	ff_licence_verify(json, strlen(json), pk, issued + 10 * 86400, &L);
	CHECK(L.state == FF_LIC_OK);
	CHECK(!strcmp(L.pack_id, "ember"));
	CHECK(!strcmp(L.discord_id, "123456789012345678"));
	CHECK(L.expires == expires);

	ff_licence_verify(json, strlen(json), pk, expires + 3 * 86400, &L);
	CHECK(L.state == FF_LIC_GRACE);
	ff_licence_verify(json, strlen(json), pk, expires + 8 * 86400, &L);
	CHECK(L.state == FF_LIC_EXPIRED);
	CHECK(strstr(L.reason, "expired") != NULL);
	ff_licence_verify(json, strlen(json), pk, expires + 7 * 86400 - 1, &L);
	CHECK(L.state == FF_LIC_GRACE);

	make(sk, "ember", issued, expires, json, sizeof json, 1);
	ff_licence_verify(json, strlen(json), pk, issued + 10, &L);
	CHECK(L.state == FF_LIC_INVALID);
	CHECK(strstr(L.reason, "signature") != NULL);

	uint8_t other_pk[32];
	uint8_t other_sk[64];
	seed[0] = 99;
	crypto_ed25519_key_pair(other_sk, other_pk, seed);
	make(sk, "ember", issued, expires, json, sizeof json, 0);
	ff_licence_verify(json, strlen(json), other_pk, issued + 10, &L);
	CHECK(L.state == FF_LIC_INVALID);

	/* tampered field after signing */
	make(sk, "ember", issued, expires, json, sizeof json, 0);
	char *p = strstr(json, "\"pack_id\":\"ember\"");
	memcpy(p + 11, "embyr", 5);
	ff_licence_verify(json, strlen(json), pk, issued + 10, &L);
	CHECK(L.state == FF_LIC_INVALID);

	const char *missing = "{\"discord_id\":\"1\",\"pack_id\":\"ember\",\"sig\":\"AAAA\"}";
	ff_licence_verify(missing, strlen(missing), pk, issued, &L);
	CHECK(L.state == FF_LIC_INVALID);
	CHECK(strstr(L.reason, "missing") != NULL);
	ff_licence_verify("not json", 8, pk, issued, &L);
	CHECK(L.state == FF_LIC_INVALID);

	char canon[256];
	size_t n = ff_licence_canonical("1", "p", "l", 5, 6, canon, sizeof canon);
	CHECK(n == strlen(canon));
	CHECK(!strcmp(canon,
		      "{\"discord_id\":\"1\",\"expires\":6,\"issued\":5,\"licence_id\":\"l\",\"pack_id\":\"p\"}"));

	/* truncated buffer (length cuts inside the sig value): must be INVALID, must not crash */
	make(sk, "ember", issued, expires, json, sizeof json, 0);
	size_t jl = strlen(json);
	ff_licence_verify(json, jl - 3, pk, issued + 10, &L);
	CHECK(L.state == FF_LIC_INVALID);

	/* non-null-terminated buffer: "expires" is the last field, and the buffer ends
	 * immediately after its final digit (no trailing comma/brace within bounds).
	 * Bytes past json_len are filled with '9' inside a malloc'd region large enough
	 * to read from safely; an unbounded strtoll would pull those '9's into the
	 * number and corrupt L.expires, which then fails signature verification. */
	{
		char canon2[512];
		size_t cn2 = ff_licence_canonical("123456789012345678", "ember", "lic_test1", issued, expires, canon2,
						  sizeof canon2);
		uint8_t sig2[64];
		crypto_ed25519_sign(sig2, sk, (const uint8_t *)canon2, cn2);
		char s642[100];
		b64(sig2, 64, s642);
		char json2[1024];
		int jl2 = snprintf(
			json2, sizeof json2,
			"{\"discord_id\":\"123456789012345678\",\"issued\":%lld,\"licence_id\":\"lic_test1\",\"pack_id\":\"ember\",\"sig\":\"%s\",\"expires\":%lld",
			(long long)issued, s642, (long long)expires);
		size_t bn = (size_t)jl2 + 8;
		char *buf = malloc(bn);
		memset(buf, '9', bn);
		memcpy(buf, json2, (size_t)jl2);
		ff_licence_verify(buf, (size_t)jl2, pk, issued + 10 * 86400, &L);
		CHECK(L.state == FF_LIC_OK);
		CHECK(L.expires == expires);
		free(buf);
	}

	FF_TEST_MAIN_END();
}
