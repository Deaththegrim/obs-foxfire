/* Drives ff-twitch-api.c against a local HTTP server, for tools/twitch-api-proof.py.
 *
 * ff_twitch_set_bases points the ID and API hosts at that server. Nothing else changes: the same
 * request building, the same JSON reading, the same error mapping runs as in production. A mock
 * that reimplemented any of that would pass while the real thing was broken.
 */

#include <ff-eventsub.h>
#include <ff-net.h>
#include <ff-twitch-api.h>
#include <obs-module.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

obs_module_t *obs_current_module(void)
{
	return NULL;
}

const char *obs_module_text(const char *key)
{
	return key;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: twitch_cli <base url> <step>\n");
		return 2;
	}
	ff_twitch_set_bases(argv[1], argv[1]);
	const char *step = argv[2];
	char err[512] = {0};

	if (!strcmp(step, "scopes")) {
		char sc[512];
		ff_twitch_scopes(sc, sizeof sc);
		printf("SCOPES:%s\n", sc);
		return 0;
	}
	if (!strcmp(step, "device")) {
		struct ff_twitch_device d;
		if (!ff_twitch_device_start("cid123", &d, err, sizeof err)) {
			printf("ERR:%s\n", err);
			return 0;
		}
		printf("CODE:%s URL:%s INTERVAL:%d\n", d.user_code, d.verify_url, d.interval);
		return 0;
	}
	if (!strcmp(step, "poll")) {
		struct ff_twitch_token t;
		enum ff_twitch_poll r = ff_twitch_device_poll("cid123", "dev123", &t, err, sizeof err);
		static const char *N[] = {"PENDING", "GOT_TOKEN", "SLOW_DOWN", "DENIED", "EXPIRED", "ERROR"};
		printf("POLL:%s", N[r]);
		if (r == FF_TW_GOT_TOKEN)
			/* > now, not > 0: a token whose expiry was stored as the DURATION
			   (14400) is still greater than zero, and would read as valid while
			   actually sitting in 1970. */
			printf(" ACCESS:%s REFRESH:%s TTL_IN_FUTURE:%d", t.access, t.refresh,
			       t.expires_at > time(NULL) + 3600);
		else
			printf(" ERR:%s", err);
		printf("\n");
		return 0;
	}
	if (!strcmp(step, "refresh")) {
		struct ff_twitch_token t;
		enum ff_refresh_result r = ff_twitch_refresh("cid123", "rt123", &t, err, sizeof err);
		static const char *N[] = {"OK", "REJECTED", "UNREACHABLE"};
		printf("REFRESH:%s %s\n", N[r], r == FF_REFRESH_OK ? t.access : err);
		return 0;
	}
	if (!strcmp(step, "user")) {
		char id[64] = {0}, login[64] = {0};
		bool ok = ff_twitch_user_id("cid123", "at123", id, sizeof id, login, sizeof login, err, sizeof err);
		printf("USER:%d ID:%s LOGIN:%s ERR:%s\n", ok, id, login, err);
		return 0;
	}
	if (!strcmp(step, "subscribe")) {
		for (size_t i = 0; i < FF_ES_SUB_COUNT; i++) {
			long status = 0;
			bool ok = ff_twitch_subscribe("cid123", "at123", &FF_ES_SUBS[i], "4242", "sess1", &status, err,
						      sizeof err);
			printf("SUB:%s OK:%d STATUS:%ld ERR:%s\n", FF_ES_SUBS[i].type, ok, status, ok ? "" : err);
		}
		return 0;
	}
	fprintf(stderr, "unknown step %s\n", step);
	return 2;
}
