#include "ff-licence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int hex(const char *h, uint8_t out[32])
{
	for (int i = 0; i < 32; i++) {
		unsigned v;
		if (sscanf(h + 2 * i, "%2x", &v) != 1)
			return 0;
		out[i] = (uint8_t)v;
	}
	return 1;
}
int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: verify_cli <pubkey-hex> <licence.json> <expected-pack-id> <pack-released-unix>\n");
		return 2;
	}
	uint8_t pk[32];
	if (!hex(argv[1], pk))
		return 2;
	FILE *f = fopen(argv[2], "rb");
	if (!f)
		return 2;
	char buf[8192];
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	struct ff_licence L;
	ff_licence_verify(buf, n, pk, argv[3], strtoll(argv[4], NULL, 10), &L);
	/* A switch, not a parallel array. The array version silently shifted every name by one when
	   the enum lost GRACE and EXPIRED and gained NEWER -- it kept printing plausible state names
	   that were simply wrong, while the reason text beside them was correct. A switch with no
	   default makes the compiler point at this line the next time a state is added. */
	const char *name = "?";
	switch (L.state) {
	case FF_LIC_NONE:
		name = "NONE";
		break;
	case FF_LIC_OK:
		name = "OK";
		break;
	case FF_LIC_NEWER:
		name = "NEWER";
		break;
	case FF_LIC_INVALID:
		name = "INVALID";
		break;
	}
	printf("%s %s\n", name, L.reason);
	return 0;
}
