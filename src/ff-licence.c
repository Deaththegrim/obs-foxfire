#define _GNU_SOURCE
#include "ff-licence.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static void *memmem(const void *h, size_t hn, const void *nd, size_t nn)
{
	const char *H = h;
	for (size_t i = 0; nn && i + nn <= hn; i++)
		if (!memcmp(H + i, nd, nn))
			return (void *)(H + i);
	return NULL;
}
#endif
/* Field getters. Return 1 when the field is present and parsed, 0 when it is
   absent, and -1 when it is present but its value is unusable. JSON permits
   whitespace around the colon, so the scanner matches the key and the colon
   separately instead of one literal "key":" pattern. */
static const char *skip_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

/* locate `"key"` followed by optional whitespace and a colon; returns the first
   character of the value, or NULL when the key is absent */
static const char *find_value(const char *j, size_t n, const char *key)
{
	char pat[80];
	int pl = snprintf(pat, sizeof pat, "\"%s\"", key);
	if (pl < 0 || (size_t)pl >= sizeof pat)
		return NULL;
	const char *p = j, *end = j + n;
	while (p < end) {
		const char *hit = memmem(p, (size_t)(end - p), pat, (size_t)pl);
		if (!hit)
			return NULL;
		const char *q = skip_ws(hit + pl, end);
		if (q < end && *q == ':')
			return skip_ws(q + 1, end);
		/* `"key"` appeared as a value, not a key -- keep looking */
		p = hit + pl;
	}
	return NULL;
}

static int get_str(const char *j, size_t n, const char *key, char *out, size_t cap)
{
	const char *end = j + n;
	const char *p = find_value(j, n, key);
	if (!p)
		return 0;
	if (p >= end || *p != '"')
		return -1;
	p++;
	const char *q = memchr(p, '"', (size_t)(end - p));
	if (!q || (size_t)(q - p) >= cap)
		return -1;
	memcpy(out, p, (size_t)(q - p));
	out[q - p] = 0;
	return 1;
}

static int get_int(const char *j, size_t n, const char *key, int64_t *out)
{
	const char *end = j + n;
	const char *p = find_value(j, n, key);
	if (!p)
		return 0;
	char num[25];
	size_t k = 0;
	if (p < end && (*p == '-' || *p == '+'))
		num[k++] = *p++;
	while (p < end && k < sizeof num - 1 && *p >= '0' && *p <= '9')
		num[k++] = *p++;
	num[k] = 0;
	char *e;
	long long v = strtoll(num, &e, 10);
	if (e == num || *e)
		return -1;
	/* The digit loop stops at the first non-digit, so trailing garbage ("12x3",
	   "12.5", a number longer than num[]) would otherwise be read as a silently
	   truncated value. Only a JSON delimiter -- or the end of the buffer, which
	   the non-null-terminated case relies on -- may follow the digits. */
	p = skip_ws(p, end);
	if (p < end && *p != ',' && *p != '}' && *p != ']')
		return -1;
	*out = v;
	return 1;
}
static int b64dec(const char *s, uint8_t *out, size_t cap)
{
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint32_t acc = 0;
	int bits = 0;
	size_t o = 0;
	for (; *s && *s != '='; s++) {
		const char *t = strchr(T, *s);
		if (!t)
			return -1;
		acc = acc << 6 | (uint32_t)(t - T);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= cap)
				return -1;
			out[o++] = (uint8_t)(acc >> bits & 0xff);
		}
	}
	return (int)o;
}

size_t ff_licence_canonical(const char *discord_id, const char *pack_id, const char *licence_id, int64_t issued,
			    int64_t entitled_through, char *buf, size_t cap)
{
	int n = snprintf(
		buf, cap,
		"{\"discord_id\":\"%s\",\"entitled_through\":%lld,\"issued\":%lld,\"licence_id\":\"%s\",\"pack_id\":\"%s\"}",
		discord_id, (long long)entitled_through, (long long)issued, licence_id, pack_id);
	return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

void ff_licence_verify(const char *json, size_t n, const uint8_t pubkey[32], const char *expected_pack_id,
		       int64_t pack_released, struct ff_licence *L)
{
	memset(L, 0, sizeof *L);
	L->state = FF_LIC_INVALID;
	char sig64[128];
	/* Field order matches the document's own so the field named in the reason is
	   the first one a reader would reach. A getter returning -1 (present but
	   unusable) must not be treated as success, which the previous `!get_str(...)
	   || ...` chain would have done. */
	const struct {
		const char *key;
		char *str;
		size_t cap;
		int64_t *num;
	} fields[] = {
		{"discord_id", L->discord_id, sizeof L->discord_id, NULL},
		{"pack_id", L->pack_id, sizeof L->pack_id, NULL},
		{"licence_id", L->licence_id, sizeof L->licence_id, NULL},
		{"issued", NULL, 0, &L->issued},
		{"entitled_through", NULL, 0, &L->entitled_through},
		{"sig", sig64, sizeof sig64, NULL},
	};
	for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
		int r = fields[i].num ? get_int(json, n, fields[i].key, fields[i].num)
				      : get_str(json, n, fields[i].key, fields[i].str, fields[i].cap);
		if (r == 1)
			continue;
		if (r == 0)
			snprintf(L->reason, sizeof L->reason, "licence file has no '%s' field", fields[i].key);
		else
			snprintf(L->reason, sizeof L->reason, "licence file's '%s' field is not a usable %s",
				 fields[i].key, fields[i].num ? "number" : "string");
		return;
	}
	uint8_t sig[64];
	if (b64dec(sig64, sig, sizeof sig) != 64) {
		snprintf(L->reason, sizeof L->reason, "licence signature is not 64 bytes of base64");
		return;
	}
	char canon[512];
	size_t cn = ff_licence_canonical(L->discord_id, L->pack_id, L->licence_id, L->issued, L->entitled_through,
					 canon, sizeof canon);
	if (!cn || crypto_ed25519_check(sig, pubkey, (const uint8_t *)canon, cn) != 0) {
		snprintf(L->reason, sizeof L->reason, "licence signature does not verify");
		return;
	}
	/* pack_id is signed (covered by canon above) so this check stops a genuine licence.json for pack A,
	   copied verbatim into pack B's folder (as shipped) -- the signature verifies, but the pack_id
	   field inside the signature is A, not B. However, this engine is GPL with public source: a user
	   who obtains the pack.json file can edit its unsigned "licensed" flag (set at src/ff-pack.c:183)
	   or "id" field, and edit the unsigned licence.json's pack_id field itself (it carries no
	   integrity protection of its own), then recompile the plugin to use their modified files. This
	   check prevents cross-package copying as a zipped pack, not local file modification. */
	if (expected_pack_id && strcmp(L->pack_id, expected_pack_id) != 0) {
		snprintf(L->reason, sizeof L->reason, "licence is for pack '%s', not '%s'", L->pack_id,
			 expected_pack_id);
		return;
	}
	/* The whole model, in one comparison. Note what is NOT here: the current time. A pack the
	   buyer is entitled to keeps working forever, including years after the subscription stops,
	   because nothing in this decision can change once both dates are fixed. */
	if (pack_released <= L->entitled_through) {
		L->state = FF_LIC_OK;
		return;
	}
	L->state = FF_LIC_NEWER;
	snprintf(L->reason, sizeof L->reason,
		 "this pack was released after your subscription ended; everything you already have keeps working");
}
