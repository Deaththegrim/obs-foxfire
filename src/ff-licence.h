#pragma once
#include <stddef.h>
#include <stdint.h>
/* FF_LIC_NEWER replaces the old GRACE/EXPIRED pair. A pack someone bought NEVER stops working;
   a lapsed subscription only withholds packs RELEASED after it lapsed. So the only "you cannot
   use this" state is "this pack is newer than what you are entitled to", which is a fact about
   two fixed dates rather than about the current time. */
enum ff_lic_state { FF_LIC_NONE, FF_LIC_OK, FF_LIC_NEWER, FF_LIC_INVALID };
struct ff_licence {
	enum ff_lic_state state;
	char discord_id[32], pack_id[64], licence_id[64];
	int64_t issued, entitled_through;
	char reason[160];
};
/* expected_pack_id: the id of the pack directory this licence file was found in. pack_id IS covered
   by the signature (see ff_licence_canonical below), so tampering with it is caught -- but nothing
   upstream of this function ever compared the signed pack_id to the pack it was found sitting next
   to, which let one valid licence.json be copied verbatim into any other paid pack's folder and
   verify there. A mismatch here is FF_LIC_INVALID with a named reason, checked only after the
   signature itself verifies (so a forged pack_id is still caught by the signature check first). */
/* pack_released: the pack's own release date (unix seconds) from its manifest. NOT the current
   time -- deliberately. The previous design compared the licence against now() and stopped a paid
   pack loading once it expired, which could brick a pack MID-STREAM at the moment a subscription
   lapsed. Comparing two fixed dates cannot fail at an awkward moment, and it matches what the
   buyer actually paid for: everything released while they were subscribed, forever.
   A pack whose manifest has no release date passes as 0, which is before every entitlement and so
   always unlocks -- older packs must never break when this field is introduced. */
void ff_licence_verify(const char *json, size_t json_len, const uint8_t pubkey[32], const char *expected_pack_id,
		       int64_t pack_released, struct ff_licence *out);
size_t ff_licence_canonical(const char *discord_id, const char *pack_id, const char *licence_id, int64_t issued,
			    int64_t entitled_through, char *buf, size_t cap);
