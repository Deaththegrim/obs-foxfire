#pragma once
#include <stddef.h>
#include <stdint.h>
enum ff_lic_state { FF_LIC_NONE, FF_LIC_OK, FF_LIC_GRACE, FF_LIC_EXPIRED, FF_LIC_INVALID };
#define FF_LIC_GRACE_SECONDS (7 * 86400)
struct ff_licence {
	enum ff_lic_state state;
	char discord_id[32], pack_id[64], licence_id[64];
	int64_t issued, expires;
	char reason[160];
};
void ff_licence_verify(const char *json, size_t json_len, const uint8_t pubkey[32], int64_t now,
		       struct ff_licence *out);
size_t ff_licence_canonical(const char *discord_id, const char *pack_id, const char *licence_id, int64_t issued,
			    int64_t expires, char *buf, size_t cap);
