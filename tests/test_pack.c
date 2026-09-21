/* Two gates live here: ff_remove_recursive() must never follow a symlink, and a licensed pack
 * must not load without a usable `released` date.
 *
 * -- ff_remove_recursive --
 *
 * Why this file exists: the install path refuses a zip carrying a symlink entry BEFORE it
 * extracts anything, so in a full end-to-end run the lstat() guard inside ff_remove_recursive()
 * is never reached. Two independent reviews of the fix found the same thing — delete the guard,
 * and the build, ctest and the headless render proof all stay green. That makes the guard a fix
 * with no gate, which is exactly the shape this project keeps getting burned by.
 *
 * So this calls ff_remove_recursive() directly, with a real symlink on a real filesystem, and
 * asserts the thing that actually matters: the canary OUTSIDE the tree survives.
 *
 * ARMED, and here is exactly how, because "I wrote a test" is not evidence it can fail. Both
 * path_is_symlink() guards were mutated out of a copy of ff-pack.c -- restoring the pre-fix walk,
 * which took directory-ness from os_dirent's stat()-derived flag and so descended into the link --
 * and this file was compiled against that copy:
 *
 *     mutant (guards removed):  12 checks, 4 failed
 *     control (real ff-pack.c): 12 checks, 0 failed
 *
 * Two of the four failures are exists(canary): the file OUTSIDE the tree was deleted. That is the
 * arbitrary-directory delete itself, reproduced on demand. Re-run that mutation if you ever touch
 * these guards.
 *
 * -- the licensed-pack `released` gate --
 *
 * Why this exists: two independent reviews found that packforge never wrote `released` at all, so
 * `released <= entitled_through` was `0 <= anything` and EVERY paid pack unlocked with ANY
 * licence. The gate is inert exactly when it is most needed, and no render proof can see it --
 * the pack loads and draws perfectly, it is just free.
 *
 * ARMED the same way. All three manifest guards in ff-pack.c (the `released` type check, the
 * `released <= 0` check and the `licensed` type check) were deleted from a copy and this file
 * compiled against it:
 *
 *     mutant (all three guards removed):  35 checks, 20 failed
 *     control (real ff-pack.c):           35 checks,  0 failed
 *
 * Three of the 35 are controls that load a VALID pack, and they pass in both runs -- without them
 * a refusal test could be green because the fixture never loads at all.
 */

#include "ff-test.h"
#include <ff-pack.h>
#include <obs-module.h>

/* OBS_DECLARE_MODULE() defines this inside the plugin, not in libobs, so a unit test that links
   ff-pack.c has to supply it. Returning NULL is safe HERE and only here: the functions that use
   it (ff_packs_scan, ff_packs_install_zip) resolve the module's bundled data path, and this test
   calls neither -- it exercises ff_remove_recursive() and ff_pack_load_dir() against paths it
   made itself, and ff_pack_load_dir() does not touch the module. If a later test in this file
   does call them, this stub must be replaced, not worked around. */
obs_module_t *obs_current_module(void)
{
	return NULL;
}

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static void join(char *out, size_t cap, const char *a, const char *b)
{
	snprintf(out, cap, "%s/%s", a, b);
}

static bool exists(const char *p)
{
	struct stat st;
	return lstat(p, &st) == 0;
}

static void write_file(const char *p, const char *text)
{
	FILE *f = fopen(p, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

/* Writes a one-preset pack whose manifest is `manifest`, loads it, and reports whether it was
   accepted. `why` receives the refusal reason so a test can assert the author is TOLD what is
   wrong rather than just seeing the pack vanish. */
static bool load_manifest(const char *base, const char *manifest, char *why, size_t cap)
{
	char dir[256], eff[512], mf[512];
	snprintf(dir, sizeof dir, "%s/packdir", base);
	mkdir(dir, 0755);
	snprintf(eff, sizeof eff, "%s/effects", dir);
	mkdir(eff, 0755);
	snprintf(eff, sizeof eff, "%s/effects/x.effect", dir);
	write_file(eff, "technique Draw { pass { } }\n");
	snprintf(mf, sizeof mf, "%s/pack.json", dir);
	write_file(mf, manifest);

	struct ff_pack_list l;
	memset(&l, 0, sizeof l);
	bool ok = ff_pack_load_dir(&l, dir);
	snprintf(why, cap, "%s", l.nerrors ? l.errors[0] : "");
	ff_packs_free(&l);
	unlink(mf);
	unlink(eff);
	snprintf(eff, sizeof eff, "%s/effects", dir);
	rmdir(eff);
	rmdir(dir);
	return ok;
}

#define PACK_HEAD "{\"format\":1,\"id\":\"t\",\"name\":\"T\",\"version\":\"1.0.0\",\"min_engine\":\"0.0.1\","
#define PACK_TAIL ",\"presets\":[{\"id\":\"p\",\"name\":\"P\",\"kind\":\"visualizer\"," \
		  "\"layers\":[{\"effect\":\"effects/x.effect\"}]}]}"

/* Every one of these reads back as 0 through obs_data_get_int(), and 0 precedes every
   entitlement date ever issued -- so before the refusal existed, each shipped a paid pack that
   unlocked for anybody holding any licence at all, silently. */
static void check_released_gate(const char *base)
{
	char why[512];

	/* Controls first. Without these the refusals below could all be passing because the
	   FIXTURE never loads, and the suite would be green while testing nothing. */
	CHECK(load_manifest(base, PACK_HEAD "\"licensed\":false" PACK_TAIL, why, sizeof why));
	CHECK(load_manifest(base, PACK_HEAD "\"licensed\":true,\"released\":1758412800" PACK_TAIL, why,
			    sizeof why));
	/* a free pack predating the field still loads: absent is back-compat, not an error */
	CHECK(load_manifest(base, PACK_HEAD "\"licensed\":false,\"x\":0" PACK_TAIL, why, sizeof why));

	static const char *bad_released[] = {
		"\"licensed\":true",                          /* the key is simply absent */
		"\"licensed\":true,\"released\":0",            /* explicitly zero */
		"\"licensed\":true,\"released\":-1",           /* before the epoch */
		"\"licensed\":true,\"released\":\"1758412800\"", /* a quoted number */
		"\"licensed\":true,\"released\":\"2026-09-21\"", /* a date string */
		"\"licensed\":true,\"released\":true",         /* a bool */
		"\"licensed\":true,\"released\":null",         /* null */
		"\"licensed\":true,\"released\":[1758412800]", /* an array */
	};
	for (size_t i = 0; i < sizeof bad_released / sizeof *bad_released; i++) {
		char json[512];
		snprintf(json, sizeof json, PACK_HEAD "%s" PACK_TAIL, bad_released[i]);
		CHECK(!load_manifest(base, json, why, sizeof why));
		CHECK(strstr(why, "released") != NULL); /* the author is told which field */
	}

	/* `licensed` itself has the same hazard in reverse: obs_data_get_bool() returns false for a
	   string or a number, so a typo would ship a paid pack as a free one with no licence check
	   and nothing said. */
	static const char *bad_licensed[] = {"\"licensed\":\"true\"", "\"licensed\":1"};
	for (size_t i = 0; i < sizeof bad_licensed / sizeof *bad_licensed; i++) {
		char json[512];
		snprintf(json, sizeof json, PACK_HEAD "%s,\"released\":1758412800" PACK_TAIL,
			 bad_licensed[i]);
		CHECK(!load_manifest(base, json, why, sizeof why));
		CHECK(strstr(why, "licensed") != NULL);
	}
}

int main(void)
{
	char base[] = "/tmp/ff-test-pack-XXXXXX";
	if (!mkdtemp(base)) {
		fprintf(stderr, "FAIL: could not create a temp dir\n");
		return 1;
	}

	char victim[512], canary[512], tree[512], normal[512], link[512], toplink[512];
	join(victim, sizeof victim, base, "victim");
	join(canary, sizeof canary, victim, "canary.txt");
	join(tree, sizeof tree, base, "tree");
	join(normal, sizeof normal, tree, "normal.txt");
	join(link, sizeof link, tree, "escape");
	join(toplink, sizeof toplink, base, "toplevel-escape");

	/* ---- case 1: a symlink to a directory, sitting INSIDE the tree being removed ---- */
	mkdir(victim, 0700);
	write_file(canary, "do not delete");
	mkdir(tree, 0700);
	write_file(normal, "ordinary file");
	CHECK(symlink(victim, link) == 0);

	CHECK(exists(canary)); /* the test is worthless if the fixture did not build */

	ff_remove_recursive(tree);

	/* The whole point. Pre-fix this file was deleted, because os_dirent's directory flag comes
	   from stat(), which follows the link, so the walk descended into victim/ and unlinked it. */
	CHECK(exists(canary));
	CHECK(!exists(tree));   /* the tree itself is still supposed to go away */
	CHECK(exists(victim));  /* and the link's TARGET directory must survive too */

	/* ---- case 2: the path handed in IS a symlink (the backup-slot case) ---- */
	CHECK(symlink(victim, toplink) == 0);
	ff_remove_recursive(toplink);
	CHECK(exists(canary));  /* removing the link must not empty what it points at */
	CHECK(exists(victim));
	CHECK(!exists(toplink)); /* the link itself should be gone */

	/* ---- case 3: an ordinary tree with no links is still fully removed ---- */
	char plain[512], deep[512], deepfile[512];
	join(plain, sizeof plain, base, "plain");
	join(deep, sizeof deep, plain, "deep");
	join(deepfile, sizeof deepfile, deep, "f.txt");
	mkdir(plain, 0700);
	mkdir(deep, 0700);
	write_file(deepfile, "x");
	CHECK(exists(deepfile));
	CHECK(ff_remove_recursive(plain));
	CHECK(!exists(plain)); /* a guard that refuses everything would fail here */

	unlink(canary);
	rmdir(victim);
	check_released_gate(base);
	rmdir(base);
	FF_TEST_MAIN_END();
}

#else /* _WIN32 */

int main(void)
{
	/* The Windows guard uses FILE_ATTRIBUTE_REPARSE_POINT and needs a privileged account or
	   developer mode to create a link at all, so it is not exercised here. The manifest gate
	   below it shares this file's POSIX temp-directory scaffolding and so goes unrun too. Say
	   so rather than reporting a pass that inspected nothing. */
	fprintf(stderr, "%s: SKIPPED on Windows -- the reparse-point guard AND the licensed-pack "
			"`released` gate are both unverified here\n", __FILE__);
	return 77;
}

#endif
