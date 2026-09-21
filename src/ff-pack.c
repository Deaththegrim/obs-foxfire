#include "ff-pack.h"
#include <plugin-support.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/pipe.h> /* os_process_pipe_ and os_process_args_ functions live here, not util/platform.h */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define ff_getpid() _getpid()
#else
#include <unistd.h>
#include <sys/stat.h>
#define ff_getpid() getpid()
#endif

/* placeholder until packforge generates the real key (Task 11 replaces this line; both halves must match) */
const uint8_t FF_PUBLIC_KEY[32] = {0};

static bool id_ok(const char *s)
{
	if (!s || !*s || strlen(s) > 63)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-'))
			return false;
	return true;
}
/* The one place a pack-relative path is judged. Exported because ff-layers.c must apply the SAME
   rule to a texture path that comes from a shader annotation -- the two used to disagree, and the
   annotation side was the weaker one, which matters most on Windows where a backslash or a drive
   letter is how you escape a directory. */
bool ff_rel_ok(const char *p)
{
	return p && *p && p[0] != '/' && !strstr(p, "..") && !strchr(p, '\\') && !strchr(p, ':');
}

/* semver_parse: strict -- true only for exactly "%d.%d.%d" with nothing trailing (the %c probe must
   fail to match because the string ends right after the third number); a missing/malformed
   min_engine therefore fails to parse rather than silently reading as 0.0.0 */
static bool semver_parse(const char *s, int *a, int *b, int *c)
{
	char extra;
	return s && sscanf(s, "%d.%d.%d%c", a, b, c, &extra) == 3;
}
static int semver_cmp3(int a1, int a2, int a3, const char *b)
{
	int b1 = 0, b2 = 0, b3 = 0;
	sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
	if (a1 != b1)
		return a1 - b1;
	if (a2 != b2)
		return a2 - b2;
	return a3 - b3;
}

static const char *path_basename(const char *p)
{
	const char *s = p;
	for (const char *c = p; *c; c++)
		if (*c == '/' || *c == '\\')
			s = c + 1;
	return s;
}

/* add_error: logs the full dir alongside a short basename, and records "pack '<basename>': <reason>"
   in errors[] -- the full dir can be very long (an install temp path), so only the basename (the pack
   id folder) goes into the fixed-size errors[] entry; the full path is still in the log line */
static void add_error(struct ff_pack_list *l, const char *dir, const char *why)
{
	const char *base = path_basename(dir);
	obs_log(LOG_WARNING, "pack '%s' (%s): %s", base, dir, why);
	if (l->nerrors < 8)
		snprintf(l->errors[l->nerrors++], sizeof l->errors[0], "pack '%.90s': %.400s", base, why);
}

static bool file_in_pack(const char *dir, const char *rel)
{
	struct dstr p = {0};
	dstr_printf(&p, "%s/%s", dir, rel);
	bool ok = os_file_exists(p.array);
	dstr_free(&p);
	return ok;
}

static bool parse_preset(struct ff_pack *pk, obs_data_t *pd, struct ff_preset *pr, char *why, size_t cap)
{
	snprintf(pr->id, sizeof pr->id, "%s", obs_data_get_string(pd, "id"));
	snprintf(pr->name, sizeof pr->name, "%s", obs_data_get_string(pd, "name"));
	snprintf(pr->kind, sizeof pr->kind, "%s", obs_data_get_string(pd, "kind"));
	snprintf(pr->thumb, sizeof pr->thumb, "%s", obs_data_get_string(pd, "thumb"));
	pr->heavy = obs_data_get_bool(pd, "heavy");
	pr->nlayers = 0;
	if (!id_ok(pr->id)) {
		snprintf(why, cap, "preset id '%s' must be [a-z0-9-]", pr->id);
		return false;
	}
	if (strcmp(pr->kind, "visualizer") && strcmp(pr->kind, "effects") && strcmp(pr->kind, "overlay")) {
		snprintf(why, cap, "preset '%s': kind must be visualizer, effects or overlay", pr->id);
		return false;
	}
	if (pr->thumb[0] && (!ff_rel_ok(pr->thumb) || !file_in_pack(pk->dir, pr->thumb))) {
		snprintf(why, cap, "preset '%s': thumb '%.300s' missing or not a relative path", pr->id, pr->thumb);
		return false;
	}
	obs_data_array_t *layers = obs_data_get_array(pd, "layers");
	size_t n = layers ? obs_data_array_count(layers) : 0;
	if (n < 1 || n > FF_MAX_LAYERS) {
		snprintf(why, cap, "preset '%s': needs 1..%d layers, has %zu", pr->id, FF_MAX_LAYERS, n);
		obs_data_array_release(layers);
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		obs_data_t *ld = obs_data_array_item(layers, i);
		struct ff_layer_def *L = &pr->layers[i];
		snprintf(L->effect_path, sizeof L->effect_path, "%s", obs_data_get_string(ld, "effect"));
		if (!ff_rel_ok(L->effect_path) || !file_in_pack(pk->dir, L->effect_path)) {
			snprintf(why, cap, "preset '%s' layer %zu: effect '%.300s' missing or not a relative path",
				 pr->id, i, L->effect_path);
			obs_data_release(ld);
			obs_data_array_release(layers);
			return false;
		}
		obs_data_t *params = obs_data_get_obj(ld, "params");
		if (params) {
			size_t cnt = 0;
			for (obs_data_item_t *it = obs_data_first(params); it; obs_data_item_next(&it))
				cnt++;
			L->params = bzalloc(sizeof(struct ff_param_override) * (cnt ? cnt : 1));
			for (obs_data_item_t *it = obs_data_first(params); it; obs_data_item_next(&it)) {
				struct ff_param_override *o = &L->params[L->nparams++];
				snprintf(o->name, sizeof o->name, "%s", obs_data_item_get_name(it));
				/* colours are objects {"r":..,"g":..,"b":..,"a":..} -- obs_data cannot parse JSON
				   arrays of bare numbers, so there is no array branch here (numbers/bools stay scalar) */
				if (obs_data_item_gettype(it) == OBS_DATA_OBJECT) {
					obs_data_t *c = obs_data_item_get_obj(it);
					o->is_color = 1;
					o->v[0] = (float)obs_data_get_double(c, "r");
					o->v[1] = (float)obs_data_get_double(c, "g");
					o->v[2] = (float)obs_data_get_double(c, "b");
					o->v[3] = obs_data_has_user_value(c, "a") ? (float)obs_data_get_double(c, "a")
										  : 1.f;
					obs_data_release(c);
				} else if (obs_data_item_gettype(it) == OBS_DATA_NUMBER) {
					o->v[0] = (float)obs_data_item_get_double(it);
				} else if (obs_data_item_gettype(it) == OBS_DATA_BOOLEAN) {
					o->v[0] = obs_data_item_get_bool(it) ? 1.f : 0.f;
				}
			}
			obs_data_release(params);
		}
		obs_data_release(ld);
		pr->nlayers = i + 1; /* layer i is now fully populated (params included); tracked incrementally
		                        so a later layer's failure lets the caller free exactly what was built */
	}
	obs_data_array_release(layers);
	return true;
}

static bool load_pack_dir(struct ff_pack_list *l, const char *dir, int64_t now)
{
	struct dstr mp = {0};
	dstr_printf(&mp, "%s/pack.json", dir);
	obs_data_t *m = obs_data_create_from_json_file(mp.array);
	dstr_free(&mp);
	if (!m) {
		add_error(l, dir, "pack.json missing or not valid JSON");
		return false;
	}
	struct ff_pack pk;
	memset(&pk, 0, sizeof pk);
	snprintf(pk.dir, sizeof pk.dir, "%s", dir);
	if ((int)obs_data_get_int(m, "format") != 1) {
		add_error(l, dir, "format must be 1");
		obs_data_release(m);
		return false;
	}
	snprintf(pk.id, sizeof pk.id, "%s", obs_data_get_string(m, "id"));
	snprintf(pk.name, sizeof pk.name, "%s", obs_data_get_string(m, "name"));
	snprintf(pk.version, sizeof pk.version, "%s", obs_data_get_string(m, "version"));
	pk.licensed = obs_data_get_bool(m, "licensed");
	if (!id_ok(pk.id)) {
		add_error(l, dir, "id must be [a-z0-9-]");
		obs_data_release(m);
		return false;
	}
	int mea = 0, meb = 0, mec = 0;
	if (!semver_parse(obs_data_get_string(m, "min_engine"), &mea, &meb, &mec)) {
		add_error(l, dir, "min_engine must be x.y.z");
		obs_data_release(m);
		return false;
	}
	if (semver_cmp3(mea, meb, mec, PLUGIN_VERSION) > 0) {
		add_error(l, dir, "needs a newer Foxfire (min_engine above this version)");
		obs_data_release(m);
		return false;
	}
	obs_data_array_t *ps = obs_data_get_array(m, "presets");
	size_t n = ps ? obs_data_array_count(ps) : 0;
	if (n < 1 || n > FF_MAX_PRESETS) {
		add_error(l, dir, "presets must have 1..64 entries");
		obs_data_array_release(ps);
		obs_data_release(m);
		return false;
	}
	pk.presets = bzalloc(sizeof(struct ff_preset) * n);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *pd = obs_data_array_item(ps, i);
		char why[512];
		if (!parse_preset(&pk, pd, &pk.presets[pk.npresets], why, sizeof why)) {
			add_error(l, dir, why);
			obs_data_release(pd);
			obs_data_array_release(ps);
			obs_data_release(m);
			/* free every preset built so far, including the in-flight (just-failed) one at index
			   pk.npresets -- parse_preset tracks nlayers incrementally so this frees exactly what
			   was allocated and nothing more */
			for (size_t k = 0; k <= pk.npresets; k++)
				for (size_t j = 0; j < pk.presets[k].nlayers; j++)
					bfree(pk.presets[k].layers[j].params);
			bfree(pk.presets);
			return false;
		}
		pk.npresets++;
		obs_data_release(pd);
	}
	obs_data_array_release(ps);
	obs_data_release(m);
	/* licence */
	pk.licence.state = FF_LIC_NONE;
	if (pk.licensed) {
		struct dstr lp = {0};
		dstr_printf(&lp, "%s/licence.json", dir);
		char *txt = os_quick_read_utf8_file(lp.array);
		dstr_free(&lp);
		if (!txt) {
			pk.licence.state = FF_LIC_INVALID;
			snprintf(pk.licence.reason, sizeof pk.licence.reason, "missing");
		} else {
			ff_licence_verify(txt, strlen(txt), FF_PUBLIC_KEY, pk.id, now, &pk.licence);
			bfree(txt);
		}
		struct dstr np = {0};
		dstr_printf(&np, "%s/licensee.json", dir);
		obs_data_t *lz = obs_data_create_from_json_file(np.array);
		dstr_free(&np);
		if (lz) {
			snprintf(pk.licensee_name, sizeof pk.licensee_name, "%s",
				 obs_data_get_string(lz, "display_name"));
			obs_data_release(lz);
		}
	}
	l->packs = brealloc(l->packs, sizeof(struct ff_pack) * (l->n + 1));
	l->packs[l->n++] = pk;
	return true;
}

static void scan_dir(struct ff_pack_list *l, const char *root, int64_t now)
{
	os_dir_t *d = os_opendir(root);
	if (!d)
		return;
	struct os_dirent *e;
	while ((e = os_readdir(d))) {
		if (!e->directory || e->d_name[0] == '.')
			continue;
		struct dstr p = {0};
		dstr_printf(&p, "%s/%s", root, e->d_name);
		load_pack_dir(l, p.array, now);
		dstr_free(&p);
	}
	os_closedir(d);
}

static bool pubkey_is_zero(void)
{
	for (int i = 0; i < 32; i++)
		if (FF_PUBLIC_KEY[i])
			return false;
	return true;
}

static void log_stale_temp_dirs(const char *packs_root)
{
	/* Scan for leftover .tmp-<pid> directories from prior crashed installs. We cannot safely
	   reclaim them: on Unix, kill(pid, 0) can return ESRCH when the process is gone, but pids can
	   be reused after wraparound, so a stale leftover whose pid now belongs to a different process
	   would be deleted catastrophically. On Windows, Process32First has no equivalent fast check.
	   Document the leak instead of silently leaking: an operator seeing "leftover temp" in the logs
	   knows to investigate. */
	os_dir_t *d = os_opendir(packs_root);
	if (!d)
		return;
	struct os_dirent *e;
	while ((e = os_readdir(d))) {
		if (e->directory && strncmp(e->d_name, ".tmp-", 5) == 0) {
			obs_log(LOG_WARNING, "packs: leftover temp directory not auto-reclaimed (cannot safely "
				 "check if its process is still alive; manual cleanup safe if pid %s is no longer running): %s",
				 e->d_name + 5, e->d_name);
		}
	}
	os_closedir(d);
}

void ff_packs_scan(struct ff_pack_list *out, int64_t now)
{
	memset(out, 0, sizeof *out);
	static bool warned_pubkey = false;
	if (!warned_pubkey && pubkey_is_zero()) {
		obs_log(LOG_WARNING, "licence: public key not set; paid packs will not verify");
		warned_pubkey = true;
	}
	char *bundled = obs_module_file("packs");
	if (bundled) {
		scan_dir(out, bundled, now);
		bfree(bundled);
	}
	char *user = obs_module_config_path("packs");
	if (user) {
		os_mkdirs(user);
		scan_dir(out, user, now);
		log_stale_temp_dirs(user);
		bfree(user);
	}
	obs_log(LOG_INFO, "packs: %zu loaded, %zu refused", out->n, out->nerrors);
}

void ff_packs_free(struct ff_pack_list *l)
{
	for (size_t i = 0; i < l->n; i++) {
		struct ff_pack *p = &l->packs[i];
		for (size_t k = 0; k < p->npresets; k++)
			for (size_t j = 0; j < p->presets[k].nlayers; j++)
				bfree(p->presets[k].layers[j].params);
		bfree(p->presets);
	}
	bfree(l->packs);
	memset(l, 0, sizeof *l);
}
const struct ff_pack *ff_packs_find(const struct ff_pack_list *l, const char *id)
{
	for (size_t i = 0; i < l->n; i++)
		if (!strcmp(l->packs[i].id, id))
			return &l->packs[i];
	return NULL;
}
const struct ff_preset *ff_pack_find_preset(const struct ff_pack *p, const char *id)
{
	if (!p)
		return NULL;
	for (size_t i = 0; i < p->npresets; i++)
		if (!strcmp(p->presets[i].id, id))
			return &p->presets[i];
	return NULL;
}

/* ---- zip install ---- */

/* path_is_symlink: the ONLY question that matters before recursing or opening a directory during
   cleanup. os_readdir()'s e->directory comes from stat(), which follows a symlink -- so a symlinked
   directory reads as a directory and ff_remove_recursive() would descend into (and delete the contents
   of) whatever it points at, which can be outside the packs tree entirely. lstat()/the reparse-point
   attribute never follow the link, so this is the one place that decision is actually safe to make.
   On any error (EACCES, ENAMETOOLONG, ELOOP, or Windows conversion/GetFileAttributes failure), this
   logs a warning and returns true (fail-closed: treat unknowns as symlinks so remove_recursive does
   not descend into them). */
#ifdef _WIN32
static bool path_is_symlink(const char *path)
{
	wchar_t *w = NULL;
	if (!os_utf8_to_wcs_ptr(path, 0, &w) || !w) {
		obs_log(LOG_WARNING, "pack cleanup: could not check if '%s' is a symlink (path conversion failed)",
			 path);
		return true;
	}
	DWORD attrs = GetFileAttributesW(w);
	DWORD gle = (attrs == INVALID_FILE_ATTRIBUTES) ? GetLastError() : 0;
	bfree(w);
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		/* the twin of the POSIX ENOENT case below: "not there" is the normal state of the
		   stale-temp slot, not an inability to tell */
		if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND)
			return false;
		obs_log(LOG_WARNING, "pack cleanup: could not check if '%s' is a symlink (GetFileAttributesW failed)",
			 path);
		return true;
	}
	return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
static bool path_is_symlink(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) {
		int err = errno;
		/* ENOENT is not a failure to determine anything -- it says the path is not there,
		   which is the ordinary state of the stale-temp slot on almost every install. Failing
		   closed here made a guard fire during completely normal operation and put a warning
		   in the log on every clean run. Anything else (EACCES on a traversal component,
		   ELOOP, ENAMETOOLONG) genuinely means "cannot tell", and that still fails closed. */
		if (err == ENOENT)
			return false;
		obs_log(LOG_WARNING, "pack cleanup: could not check if '%s' is a symlink (lstat failed: %s)",
			 path, strerror(err));
		return true;
	}
	return S_ISLNK(st.st_mode);
}
#endif

bool ff_remove_recursive(const char *path)
{
	/* path itself may be a symlink (e.g. a symlinked pack directory renamed straight into the
	   backup slot on reinstall, see ff_packs_install_zip): unlink it and stop, never opendir()
	   it -- opendir() on a symlink-to-directory follows the link exactly like stat() does. */
	if (path_is_symlink(path))
		return os_unlink(path) == 0 || !os_file_exists(path);
	os_dir_t *d = os_opendir(path);
	if (!d)
		return os_rmdir(path) == 0 || !os_file_exists(path);
	struct os_dirent *e;
	bool ok = true;
	while ((e = os_readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		struct dstr p = {0};
		dstr_printf(&p, "%s/%s", path, e->d_name);
		if (path_is_symlink(p.array)) {
			if (os_unlink(p.array) != 0)
				ok = false;
		} else if (e->directory) {
			if (!ff_remove_recursive(p.array))
				ok = false;
		} else if (os_unlink(p.array) != 0)
			ok = false;
		dstr_free(&p);
	}
	os_closedir(d);
	if (os_rmdir(path) != 0)
		ok = false;
	return ok;
}

/* run_args_capture: runs a command built with the os_process_args_t argv API (never a shell string --
   a path or zip entry containing '"', '$(', backticks etc. is just an argv element, not shell syntax) */
static bool run_args_capture(os_process_args_t *args, struct dstr *out)
{
	dstr_free(out);
	os_process_pipe_t *pp = os_process_pipe_create2(args, "r");
	if (!pp)
		return false;
	uint8_t buf[4096];
	size_t n;
	while ((n = os_process_pipe_read(pp, buf, sizeof buf)) > 0)
		dstr_ncat(out, (const char *)buf, n);
	int rc = os_process_pipe_destroy(pp);
	return rc == 0;
}

/* first_entry_top: the top-level folder is derived from the FIRST listed entry's text up to its
   first '/' -- packforge's zips list files directly (e.g. "ember/pack.json") with no explicit
   "ember/" directory record, so requiring a literal directory entry would refuse every real pack */
static bool first_entry_top(const char *line, size_t len, struct dstr *top)
{
	const char *slash = memchr(line, '/', len);
	if (!slash)
		return false;
	size_t tlen = (size_t)(slash - line);
	if (!tlen)
		return false;
	for (size_t i = 0; i < tlen; i++) {
		char c = line[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
			return false;
	}
	dstr_ncopy(top, line, tlen);
	return true;
}

/* check_zip_entries: every listed entry must start with "<top>/" and contain no ".." path segment,
   so an installed pack can never write outside packs/<id>/ -- checked per-entry, not just the first,
   since a prefix match alone would not catch a ".." segment further into the same entry */
static bool check_zip_entries(const struct dstr *listing, struct dstr *top, char *msg, size_t cap)
{
	const char *p = listing->array, *end = p + listing->len;
	bool have_top = false;
	while (p < end) {
		const char *lend = memchr(p, '\n', (size_t)(end - p));
		size_t ll = lend ? (size_t)(lend - p) : (size_t)(end - p);
		while (ll && p[ll - 1] == '\r')
			ll--;
		if (ll > 0) {
			if (!have_top) {
				if (!first_entry_top(p, ll, top)) {
					snprintf(
						msg, cap,
						"zip must contain exactly one top-level folder named like the pack id");
					return false;
				}
				have_top = true;
			}
			bool bad = ll < top->len + 1 || p[top->len] != '/' || strncmp(p, top->array, top->len) != 0;
			if (!bad)
				for (size_t i = 0; i + 1 < ll; i++)
					if (p[i] == '.' && p[i + 1] == '.' && (i == 0 || p[i - 1] == '/') &&
					    (i + 2 == ll || p[i + 2] == '/')) {
						bad = true;
						break;
					}
			if (bad) {
				snprintf(msg, cap,
					 "zip must contain exactly one top-level folder named like the pack id");
				return false;
			}
		}
		if (!lend)
			break;
		p = lend + 1;
	}
	if (!have_top) {
		snprintf(msg, cap, "zip is empty");
		return false;
	}
	return true;
}

/* listing_has_symlink: check_zip_entries above judges entry NAMES only -- a symlink entry with an
   in-tree name (e.g. "ember/link") passes every one of those checks and would be restored by the
   extractor as an actual symlink inside the extraction directory, which is exactly what lets
   ff_remove_recursive() (or anything else that later walks the pack) be steered outside packs/<id>/.
   The type character libobs never gives us via os_readdir()/os_dirent (that struct doesn't carry
   it) is the first column of a VERBOSE zip/tar listing -- 'l' for a symlink, the same convention as
   `ls -l` -- so this reads a second, verbose listing of the same static file purely to check that
   one column, one line at a time, and never splits a line on whitespace (a name containing spaces
   would corrupt a field-split, but the type character is always column zero regardless). */
static bool listing_has_symlink(const struct dstr *verbose_listing)
{
	if (!verbose_listing || !verbose_listing->array || verbose_listing->len == 0)
		return false;
	const char *p = verbose_listing->array, *end = p + verbose_listing->len;
	while (p < end) {
		if (*p == 'l')
			return true;
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		p = nl ? nl + 1 : end;
	}
	return false;
}

/* find_on_path: os_process_args_create()/os_process_pipe_create2() on this libobs build take the
   executable exactly as given -- no PATH search like execvp -- so a bare "unzip"/"tar" silently
   fails (os_process_pipe_create2 returns NULL). Resolve an absolute path ourselves by walking $PATH
   and checking for the file; this is plain directory/file lookup, not shell parsing, so it does not
   reopen the M5 shell-injection surface. */
static bool find_on_path(const char *prog, struct dstr *out)
{
#ifdef _WIN32
	const char sep = ';';
#else
	const char sep = ':';
#endif
	const char *path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/bin:/bin";
	struct dstr dirs = {0};
	dstr_copy(&dirs, path);
	bool found = false;
	char *save = dirs.array;
	while (save && *save) {
		char *next = strchr(save, sep);
		if (next)
			*next = 0;
		if (*save) {
			struct dstr cand = {0};
			dstr_printf(&cand, "%s/%s", save, prog);
			if (os_file_exists(cand.array)) {
				dstr_copy(out, cand.array);
				found = true;
			}
#ifdef _WIN32
			if (!found) {
				struct dstr cand_exe = {0};
				dstr_printf(&cand_exe, "%s.exe", cand.array);
				if (os_file_exists(cand_exe.array)) {
					dstr_copy(out, cand_exe.array);
					found = true;
				}
				dstr_free(&cand_exe);
			}
#endif
			dstr_free(&cand);
		}
		if (found)
			break;
		save = next ? next + 1 : NULL;
	}
	dstr_free(&dirs);
	return found;
}

bool ff_packs_install_zip(const char *zip_path, char *msg, size_t cap)
{
	msg[0] = 0;
	if (!zip_path || !os_file_exists(zip_path)) {
		snprintf(msg, cap, "zip file not found");
		obs_log(LOG_WARNING, "pack install: zip file not found: %s", zip_path ? zip_path : "(null)");
		return false;
	}

	char *packs_dir = obs_module_config_path("packs");
	if (!packs_dir) {
		snprintf(msg, cap, "could not resolve the packs directory");
		obs_log(LOG_WARNING, "pack install: could not resolve the packs directory");
		return false;
	}
	os_mkdirs(packs_dir);

#ifdef _WIN32
	const char *toolname = "tar";
#else
	const char *toolname = "unzip";
#endif
	struct dstr tool = {0};
	if (!find_on_path(toolname, &tool)) {
		snprintf(msg, cap, "'%s' was not found on PATH", toolname);
		obs_log(LOG_WARNING, "pack install: '%s' not found on PATH", toolname);
		dstr_free(&tool);
		bfree(packs_dir);
		return false;
	}

#ifdef _WIN32
	os_process_args_t *list_args = os_process_args_create(tool.array);
	os_process_args_add_arg(list_args, "-tf");
	os_process_args_add_arg(list_args, zip_path);
#else
	os_process_args_t *list_args = os_process_args_create(tool.array);
	os_process_args_add_arg(list_args, "-Z1");
	os_process_args_add_arg(list_args, zip_path);
#endif
	struct dstr listing = {0};
	bool listed = run_args_capture(list_args, &listing) && listing.len > 0;
	os_process_args_destroy(list_args);
	if (!listed) {
		snprintf(msg, cap, "could not read the zip's contents");
		obs_log(LOG_WARNING, "pack install: could not list zip contents: %s", zip_path);
		dstr_free(&listing);
		dstr_free(&tool);
		bfree(packs_dir);
		return false;
	}

	struct dstr topdir = {0};
	bool topdir_valid = check_zip_entries(&listing, &topdir, msg, cap);
	dstr_free(&listing);
	if (!topdir_valid) {
		obs_log(LOG_WARNING, "pack install: %s (%s)", msg, zip_path);
		dstr_free(&topdir);
		dstr_free(&tool);
		bfree(packs_dir);
		return false;
	}

	/* Second pass: a VERBOSE listing of the same static zip, read purely to refuse a symlink entry
	   (see listing_has_symlink above) -- check_zip_entries above already accepted this zip on
	   names alone, so this is the "listing step" catching what name-only checking cannot. */
#ifdef _WIN32
	os_process_args_t *type_args = os_process_args_create(tool.array);
	os_process_args_add_arg(type_args, "-tvf");
	os_process_args_add_arg(type_args, zip_path);
#else
	os_process_args_t *type_args = os_process_args_create(tool.array);
	os_process_args_add_arg(type_args, "-Z");
	os_process_args_add_arg(type_args, zip_path);
#endif
	struct dstr type_listing = {0};
	bool type_listed = run_args_capture(type_args, &type_listing) && type_listing.len > 0;
	os_process_args_destroy(type_args);
	if (!type_listed) {
		snprintf(msg, cap, "could not check the zip for invalid entries (symlinks)");
		obs_log(LOG_WARNING, "pack install: could not read verbose zip listing: %s", zip_path);
		dstr_free(&type_listing);
		dstr_free(&topdir);
		dstr_free(&tool);
		bfree(packs_dir);
		return false;
	}
	bool has_symlink = listing_has_symlink(&type_listing);
	dstr_free(&type_listing);
	if (has_symlink) {
		snprintf(msg, cap, "zip must not contain symlinks. Re-download from kitsune.gg if this is an official pack.");
		obs_log(LOG_WARNING, "pack install: zip contains a symlink entry, refused: %s", zip_path);
		dstr_free(&topdir);
		dstr_free(&tool);
		bfree(packs_dir);
		return false;
	}

	struct dstr tmp = {0};
	dstr_printf(&tmp, "%s/.tmp-%d", packs_dir, (int)ff_getpid());
	if (!ff_remove_recursive(tmp.array)) /* clear any stale leftover from a crashed prior install */
		obs_log(LOG_WARNING, "pack install: failed to clean up leftover temp directory: %s", tmp.array);
	os_mkdirs(tmp.array);

#ifdef _WIN32
	os_process_args_t *ex_args = os_process_args_create(tool.array);
	os_process_args_add_arg(ex_args, "-xf");
	os_process_args_add_arg(ex_args, zip_path);
	os_process_args_add_arg(ex_args, "-C");
	os_process_args_add_arg(ex_args, tmp.array);
#else
	os_process_args_t *ex_args = os_process_args_create(tool.array);
	os_process_args_add_arg(ex_args, "-o");
	os_process_args_add_arg(ex_args, "-q");
	os_process_args_add_arg(ex_args, "-d");
	os_process_args_add_arg(ex_args, tmp.array);
	os_process_args_add_arg(ex_args, zip_path);
#endif
	struct dstr extract_out = {0};
	bool extracted = run_args_capture(ex_args, &extract_out);
	os_process_args_destroy(ex_args);
	dstr_free(&extract_out);
	dstr_free(&tool);
	if (!extracted) {
		snprintf(msg, cap, "failed to extract the zip");
		obs_log(LOG_WARNING, "pack install: failed to extract %s", zip_path);
		if (!ff_remove_recursive(tmp.array))
			obs_log(LOG_WARNING, "pack install: failed to clean up extraction temp dir: %s", tmp.array);
		dstr_free(&tmp);
		dstr_free(&topdir);
		bfree(packs_dir);
		return false;
	}

	struct dstr pack_src = {0};
	dstr_printf(&pack_src, "%s/%.*s", tmp.array, (int)topdir.len, topdir.array);
	dstr_free(&topdir);

	struct ff_pack_list check;
	memset(&check, 0, sizeof check);
	bool ok = load_pack_dir(&check, pack_src.array, (int64_t)time(NULL));
	if (!ok) {
		snprintf(msg, cap, "%s", check.nerrors ? check.errors[0] : "pack failed validation");
		ff_packs_free(&check);
		if (!ff_remove_recursive(tmp.array))
			obs_log(LOG_WARNING, "pack install: failed to clean up extraction temp dir after validation failure: %s", tmp.array);
		dstr_free(&tmp);
		dstr_free(&pack_src);
		bfree(packs_dir);
		return false;
	}

	char id[64];
	snprintf(id, sizeof id, "%s", check.packs[0].id);
	ff_packs_free(&check);

	/* never delete an existing same-id pack before the new one is safely in place: os_safe_replace
	   renames dest aside to a backup, moves pack_src into dest, and on failure restores dest from the
	   backup -- so a failed reinstall never loses the pack that was already there */
	struct dstr dest = {0};
	dstr_printf(&dest, "%s/%s", packs_dir, id);
	bool moved;
	if (os_file_exists(dest.array)) {
		struct dstr backup = {0};
		dstr_printf(&backup, "%s/.bak-%s-%d", packs_dir, id, (int)ff_getpid());
		if (!ff_remove_recursive(backup.array)) /* clear any stale leftover backup from a crashed prior install */
			obs_log(LOG_WARNING, "pack install: failed to clean up old backup dir: %s", backup.array);
		moved = os_safe_replace(dest.array, pack_src.array, backup.array) == 0;
		if (moved)
			ff_remove_recursive(
				backup.array); /* on failure os_safe_replace has restored dest; leave backup for inspection */
		dstr_free(&backup);
	} else {
		moved = os_rename(pack_src.array, dest.array) == 0;
	}
	dstr_free(&dest);

	if (moved) {
		if (!ff_remove_recursive(tmp.array)) /* pack_src was moved out of tmp; the wrapper dir is safe to clear now */
			obs_log(LOG_WARNING, "pack install: failed to clean up wrapper temp dir after move: %s", tmp.array);
		dstr_free(&tmp);
		dstr_free(&pack_src);
		bfree(packs_dir);
		snprintf(msg, cap, "installed '%s'", id);
		obs_log(LOG_INFO, "pack install: installed '%s'", id);
		return true;
	}

	/* the move failed: dest (if it existed) was restored by os_safe_replace, or was never touched in
	   the plain-rename case -- never remove tmp here, it still holds the only copy of the validated pack */
	dstr_free(&tmp);
	dstr_free(&pack_src);
	bfree(packs_dir);
	snprintf(msg, cap, "failed to install pack '%s' into place", id);
	obs_log(LOG_WARNING, "pack install: rename failed for '%s'", id);
	return false;
}
