/* ff_remove_recursive() must never follow a symlink.
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
 */

#include "ff-test.h"
#include <ff-pack.h>
#include <obs-module.h>

/* OBS_DECLARE_MODULE() defines this inside the plugin, not in libobs, so a unit test that links
   ff-pack.c has to supply it. Returning NULL is safe HERE and only here: the functions that use
   it (ff_packs_scan, ff_packs_install_zip) resolve the module's bundled data path, and this test
   calls neither -- it exercises ff_remove_recursive() against paths it made itself. If a later
   test in this file does call them, this stub must be replaced, not worked around. */
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
	rmdir(base);
	FF_TEST_MAIN_END();
}

#else /* _WIN32 */

int main(void)
{
	/* The Windows guard uses FILE_ATTRIBUTE_REPARSE_POINT and needs a privileged account or
	   developer mode to create a link at all, so it is not exercised here. Say so rather than
	   reporting a pass that inspected nothing. */
	fprintf(stderr, "%s: SKIPPED on Windows -- reparse-point guard is unverified\n", __FILE__);
	return 77;
}

#endif
