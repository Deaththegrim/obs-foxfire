/* Where the refresh token lives, and who can read it.
 *
 * This had no test of any kind. It is the one file this plugin writes that is a CREDENTIAL:
 * anything that can read it can post to Twitch as the streamer. Three properties matter and none
 * of them are visible from the outside:
 *
 *   1. the file is owner-only. obs_data_save_json_safe creates with the process umask, which on a
 *      default Linux desktop is world-readable -- so the chmod AFTER the save is load-bearing,
 *      and a future edit that drops it would change nothing anyone could see.
 *   2. the ACCESS token is deliberately not written. It lasts four hours and can always be minted
 *      again; one fewer credential on disk. Nothing pinned that, so a "save everything" edit
 *      would have been silent.
 *   3. sign-out actually deletes it.
 *
 * ARMED by mutation -- table at the end.
 */

#include "ff-test.h"
#include <ff-twitch-api.h>
#include <obs-module.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

obs_module_t *obs_current_module(void)
{
	return NULL;
}

const char *obs_module_text(const char *key)
{
	return key;
}

/* ff_shared_config_path anchors on a "plugin_config" component, which is what makes both plugins
   share one directory. The path handed in here is shaped like a real one. */
static void make_root(char *out, size_t cap)
{
	snprintf(out, cap, "/tmp/ff-token-test-%d/obs-studio/plugin_config/obs-foxfire/packs",
		 (int)getpid());
	char mk[1024];
	snprintf(mk, sizeof mk, "/tmp/ff-token-test-%d/obs-studio/plugin_config/foxfire",
		 (int)getpid());
	char cmd[1200];
	snprintf(cmd, sizeof cmd, "mkdir -p '%s'", mk);
	if (system(cmd) != 0)
		fprintf(stderr, "could not create %s\n", mk);
}

static bool slurp(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	size_t n = fread(out, 1, cap - 1, f);
	out[n] = 0;
	fclose(f);
	return true;
}

int main(void)
{
	char root[1024];
	make_root(root, sizeof root);
	char path[1200];
	snprintf(path, sizeof path, "/tmp/ff-token-test-%d/obs-studio/plugin_config/foxfire/twitch.json",
		 (int)getpid());

	struct ff_twitch_token t;
	memset(&t, 0, sizeof t);
	snprintf(t.access, sizeof t.access, "%s", "AN-ACCESS-TOKEN-THAT-MUST-NOT-BE-WRITTEN");
	snprintf(t.refresh, sizeof t.refresh, "%s", "a-refresh-token");
	snprintf(t.scopes, sizeof t.scopes, "%s", "bits:read channel:read:subscriptions");

	CHECK(ff_twitch_token_save(root, &t));

	/* 1. owner-only */
	struct stat st;
	CHECK(stat(path, &st) == 0);
	CHECK((st.st_mode & 0777) == 0600);
	if ((st.st_mode & 0777) != 0600)
		fprintf(stderr, "      (mode is %04o; a credential readable by other accounts)\n",
			st.st_mode & 0777);

	/* 2. the access token is not on disk -- neither the value nor a key to put one in */
	char body[8192] = {0};
	CHECK(slurp(path, body, sizeof body));
	CHECK(strstr(body, "AN-ACCESS-TOKEN-THAT-MUST-NOT-BE-WRITTEN") == NULL);
	CHECK(strstr(body, "access") == NULL);
	CHECK(strstr(body, "a-refresh-token") != NULL);

	/* it round-trips */
	struct ff_twitch_token back;
	CHECK(ff_twitch_token_load(root, &back));
	CHECK(strcmp(back.refresh, "a-refresh-token") == 0);
	CHECK(strcmp(back.scopes, "bits:read channel:read:subscriptions") == 0);
	CHECK(back.access[0] == 0); /* nothing to load it from, and nothing invented */

	/* 3. sign-out really deletes it */
	ff_twitch_token_forget(root);
	CHECK(stat(path, &st) != 0);
	CHECK(!ff_twitch_token_load(root, &back));

	/* A file with no refresh token in it is not a sign-in. Loading it as one sends the worker
	   round its loop with nothing to refresh -- which is the state that used to spin a core. */
	FILE *f = fopen(path, "w");
	if (f) {
		fputs("{\"scopes\":\"bits:read\"}", f);
		fclose(f);
	}
	CHECK(!ff_twitch_token_load(root, &back));

	/* A path with no plugin_config component cannot be shared between the two plugins, so it
	   is refused rather than written somewhere only one of them will look. */
	CHECK(!ff_twitch_token_save("/tmp", &t));
	CHECK(!ff_twitch_token_load("/tmp", &back));

	ff_twitch_token_forget(root);
	char rm[1200];
	snprintf(rm, sizeof rm, "rm -rf '/tmp/ff-token-test-%d'", (int)getpid());
	if (system(rm) != 0)
		fprintf(stderr, "could not clean up\n");

	FF_TEST_MAIN_END();
}

/* Measured, each guard removed in turn, recompiled and rerun:
 *
 *     control (every guard in place)           16 checks,  0 failed
 *     chmod 0600 removed                       16 checks,  1 failed
 *     access token also written to the file    16 checks,  2 failed
 *     empty-refresh check removed on load      16 checks,  1 failed
 *     path anchoring removed (writes anywhere) 16 checks, 10 failed
 */
