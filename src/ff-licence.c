#define _GNU_SOURCE
#include "ff-licence.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static void *memmem(const void *h, size_t hn, const void *nd, size_t nn) { const char *H = h; for (size_t i = 0; nn && i + nn <= hn; i++) if (!memcmp(H + i, nd, nn)) return (void *)(H + i); return NULL; }
#endif

/* find "key":<value>; copies a string value (without quotes) or an integer; returns 0 if absent */
static int get_str(const char *j, size_t n, const char *key, char *out, size_t cap)
{
	char pat[80]; int pl = snprintf(pat, sizeof pat, "\"%s\":\"", key);
	const char *p = j, *end = j + n;
	while ((p = memmem(p, (size_t)(end - p), pat, (size_t)pl))) {
		p += pl; const char *q = memchr(p, '"', (size_t)(end - p));
		if (!q || (size_t)(q - p) >= cap) return 0;
		memcpy(out, p, (size_t)(q - p)); out[q - p] = 0; return 1;
	}
	return 0;
}
static int get_int(const char *j, size_t n, const char *key, int64_t *out)
{
	char pat[80]; int pl = snprintf(pat, sizeof pat, "\"%s\":", key);
	const char *p = memmem(j, n, pat, (size_t)pl);
	if (!p) return 0;
	p += pl; if (*p == '"') return 0;
	char *e; long long v = strtoll(p, &e, 10);
	if (e == p) return 0;
	*out = v; return 1;
}
static int b64dec(const char *s, uint8_t *out, size_t cap)
{
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint32_t acc = 0; int bits = 0; size_t o = 0;
	for (; *s && *s != '='; s++) {
		const char *t = strchr(T, *s); if (!t) return -1;
		acc = acc << 6 | (uint32_t)(t - T); bits += 6;
		if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)(acc >> bits & 0xff); }
	}
	return (int)o;
}

size_t ff_licence_canonical(const char *discord_id, const char *pack_id, const char *licence_id, int64_t issued, int64_t expires, char *buf, size_t cap)
{
	int n = snprintf(buf, cap, "{\"discord_id\":\"%s\",\"expires\":%lld,\"issued\":%lld,\"licence_id\":\"%s\",\"pack_id\":\"%s\"}",
	                 discord_id, (long long)expires, (long long)issued, licence_id, pack_id);
	return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

void ff_licence_verify(const char *json, size_t n, const uint8_t pubkey[32], int64_t now, struct ff_licence *L)
{
	memset(L, 0, sizeof *L); L->state = FF_LIC_INVALID;
	char sig64[128];
	if (!get_str(json, n, "discord_id", L->discord_id, sizeof L->discord_id) || !get_str(json, n, "pack_id", L->pack_id, sizeof L->pack_id) ||
	    !get_str(json, n, "licence_id", L->licence_id, sizeof L->licence_id) || !get_int(json, n, "issued", &L->issued) ||
	    !get_int(json, n, "expires", &L->expires) || !get_str(json, n, "sig", sig64, sizeof sig64)) {
		snprintf(L->reason, sizeof L->reason, "licence file is missing a field (discord_id, pack_id, licence_id, issued, expires, sig)"); return;
	}
	uint8_t sig[64]; if (b64dec(sig64, sig, sizeof sig) != 64) { snprintf(L->reason, sizeof L->reason, "licence signature is not 64 bytes of base64"); return; }
	char canon[512]; size_t cn = ff_licence_canonical(L->discord_id, L->pack_id, L->licence_id, L->issued, L->expires, canon, sizeof canon);
	if (!cn || crypto_ed25519_check(sig, pubkey, (const uint8_t *)canon, cn) != 0) { snprintf(L->reason, sizeof L->reason, "licence signature does not verify"); return; }
	if (now <= L->expires) { L->state = FF_LIC_OK; return; }
	if (now <= L->expires + FF_LIC_GRACE_SECONDS) { L->state = FF_LIC_GRACE; snprintf(L->reason, sizeof L->reason, "licence expired; grace period, renew on kitsune.gg"); return; }
	L->state = FF_LIC_EXPIRED; snprintf(L->reason, sizeof L->reason, "licence expired on %lld; renew on kitsune.gg/art/gear.html", (long long)L->expires);
}
