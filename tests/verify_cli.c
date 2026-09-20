#include "ff-licence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int hex(const char *h, uint8_t out[32]) { for (int i = 0; i < 32; i++) { unsigned v; if (sscanf(h + 2 * i, "%2x", &v) != 1) return 0; out[i] = (uint8_t)v; } return 1; }
int main(int argc, char **argv)
{
	if (argc != 4) { fprintf(stderr, "usage: verify_cli <pubkey-hex> <licence.json> <now-unix>\n"); return 2; }
	uint8_t pk[32]; if (!hex(argv[1], pk)) return 2;
	FILE *f = fopen(argv[2], "rb"); if (!f) return 2;
	char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
	struct ff_licence L; ff_licence_verify(buf, n, pk, strtoll(argv[3], NULL, 10), &L);
	static const char *names[] = { "NONE", "OK", "GRACE", "EXPIRED", "INVALID" };
	printf("%s %s\n", names[L.state], L.reason); return 0;
}
