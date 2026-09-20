#include "ff-pack.h"
#include <plugin-support.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/pipe.h> /* os_process_pipe_* live here, not util/platform.h */
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#define ff_getpid() _getpid()
#else
#include <unistd.h>
#define ff_getpid() getpid()
#endif

/* placeholder until packforge generates the real key (Task 11 replaces this line; both halves must match) */
const uint8_t FF_PUBLIC_KEY[32] = {0};

static bool id_ok(const char *s) { if (!s || !*s || strlen(s) > 63) return false; for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false; return true; }
static bool rel_ok(const char *p) { return p && *p && p[0] != '/' && !strstr(p, "..") && !strchr(p, '\\') && !strchr(p, ':'); }
static int semver_cmp(const char *a, const char *b) { int a1=0,a2=0,a3=0,b1=0,b2=0,b3=0; sscanf(a, "%d.%d.%d", &a1,&a2,&a3); sscanf(b, "%d.%d.%d", &b1,&b2,&b3); if (a1!=b1) return a1-b1; if (a2!=b2) return a2-b2; return a3-b3; }

static void add_error(struct ff_pack_list *l, const char *dir, const char *why)
{
	obs_log(LOG_WARNING, "pack '%s': %s", dir, why);
	if (l->nerrors < 8) snprintf(l->errors[l->nerrors++], 256, "pack '%.100s': %.140s", dir, why);
}

static bool file_in_pack(const char *dir, const char *rel) { struct dstr p = {0}; dstr_printf(&p, "%s/%s", dir, rel); bool ok = os_file_exists(p.array); dstr_free(&p); return ok; }

static bool parse_preset(struct ff_pack *pk, obs_data_t *pd, struct ff_preset *pr, char *why, size_t cap)
{
	snprintf(pr->id, sizeof pr->id, "%s", obs_data_get_string(pd, "id"));
	snprintf(pr->name, sizeof pr->name, "%s", obs_data_get_string(pd, "name"));
	snprintf(pr->kind, sizeof pr->kind, "%s", obs_data_get_string(pd, "kind"));
	snprintf(pr->thumb, sizeof pr->thumb, "%s", obs_data_get_string(pd, "thumb"));
	pr->heavy = obs_data_get_bool(pd, "heavy");
	if (!id_ok(pr->id)) { snprintf(why, cap, "preset id '%s' must be [a-z0-9-]", pr->id); return false; }
	if (strcmp(pr->kind, "visualizer") && strcmp(pr->kind, "effects") && strcmp(pr->kind, "overlay")) { snprintf(why, cap, "preset '%s': kind must be visualizer, effects or overlay", pr->id); return false; }
	if (pr->thumb[0] && (!rel_ok(pr->thumb) || !file_in_pack(pk->dir, pr->thumb))) { snprintf(why, cap, "preset '%s': thumb '%.120s' missing or not a relative path", pr->id, pr->thumb); return false; }
	obs_data_array_t *layers = obs_data_get_array(pd, "layers");
	size_t n = layers ? obs_data_array_count(layers) : 0;
	if (n < 1 || n > FF_MAX_LAYERS) { snprintf(why, cap, "preset '%s': needs 1..%d layers, has %zu", pr->id, FF_MAX_LAYERS, n); obs_data_array_release(layers); return false; }
	for (size_t i = 0; i < n; i++) {
		obs_data_t *ld = obs_data_array_item(layers, i);
		struct ff_layer_def *L = &pr->layers[i];
		snprintf(L->effect_path, sizeof L->effect_path, "%s", obs_data_get_string(ld, "effect"));
		if (!rel_ok(L->effect_path) || !file_in_pack(pk->dir, L->effect_path)) { snprintf(why, cap, "preset '%s' layer %zu: effect '%.90s' missing or not a relative path", pr->id, i, L->effect_path); obs_data_release(ld); obs_data_array_release(layers); return false; }
		obs_data_t *params = obs_data_get_obj(ld, "params");
		if (params) {
			size_t cnt = 0; for (obs_data_item_t *it = obs_data_first(params); it; obs_data_item_next(&it)) cnt++;
			L->params = bzalloc(sizeof(struct ff_param_override) * (cnt ? cnt : 1));
			for (obs_data_item_t *it = obs_data_first(params); it; obs_data_item_next(&it)) {
				struct ff_param_override *o = &L->params[L->nparams++];
				snprintf(o->name, sizeof o->name, "%s", obs_data_item_get_name(it));
				/* colours are objects {"r":..,"g":..,"b":..,"a":..} -- obs_data cannot parse JSON
				   arrays of bare numbers, so there is no array branch here (numbers/bools stay scalar) */
				if (obs_data_item_gettype(it) == OBS_DATA_OBJECT) {
					obs_data_t *c = obs_data_item_get_obj(it); o->is_color = 1;
					o->v[0] = (float)obs_data_get_double(c, "r"); o->v[1] = (float)obs_data_get_double(c, "g"); o->v[2] = (float)obs_data_get_double(c, "b"); o->v[3] = obs_data_has_user_value(c, "a") ? (float)obs_data_get_double(c, "a") : 1.f;
					obs_data_release(c);
				} else if (obs_data_item_gettype(it) == OBS_DATA_NUMBER) { o->v[0] = (float)obs_data_item_get_double(it); }
				else if (obs_data_item_gettype(it) == OBS_DATA_BOOLEAN) { o->v[0] = obs_data_item_get_bool(it) ? 1.f : 0.f; }
			}
			obs_data_release(params);
		}
		obs_data_release(ld);
	}
	pr->nlayers = n;
	obs_data_array_release(layers);
	return true;
}

static bool load_pack_dir(struct ff_pack_list *l, const char *dir, int64_t now)
{
	struct dstr mp = {0}; dstr_printf(&mp, "%s/pack.json", dir);
	obs_data_t *m = obs_data_create_from_json_file(mp.array); dstr_free(&mp);
	if (!m) { add_error(l, dir, "pack.json missing or not valid JSON"); return false; }
	struct ff_pack pk; memset(&pk, 0, sizeof pk);
	snprintf(pk.dir, sizeof pk.dir, "%s", dir);
	if ((int)obs_data_get_int(m, "format") != 1) { add_error(l, dir, "format must be 1"); obs_data_release(m); return false; }
	snprintf(pk.id, sizeof pk.id, "%s", obs_data_get_string(m, "id"));
	snprintf(pk.name, sizeof pk.name, "%s", obs_data_get_string(m, "name"));
	snprintf(pk.version, sizeof pk.version, "%s", obs_data_get_string(m, "version"));
	pk.licensed = obs_data_get_bool(m, "licensed");
	if (!id_ok(pk.id)) { add_error(l, dir, "id must be [a-z0-9-]"); obs_data_release(m); return false; }
	if (semver_cmp(obs_data_get_string(m, "min_engine"), PLUGIN_VERSION) > 0) { add_error(l, dir, "needs a newer Foxfire (min_engine above this version)"); obs_data_release(m); return false; }
	obs_data_array_t *ps = obs_data_get_array(m, "presets"); size_t n = ps ? obs_data_array_count(ps) : 0;
	if (n < 1 || n > FF_MAX_PRESETS) { add_error(l, dir, "presets must have 1..64 entries"); obs_data_array_release(ps); obs_data_release(m); return false; }
	pk.presets = bzalloc(sizeof(struct ff_preset) * n);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *pd = obs_data_array_item(ps, i); char why[256];
		if (!parse_preset(&pk, pd, &pk.presets[pk.npresets], why, sizeof why)) { add_error(l, dir, why); obs_data_release(pd); obs_data_array_release(ps); obs_data_release(m); /* free presets */ for (size_t k = 0; k < pk.npresets; k++) for (size_t j = 0; j < FF_MAX_LAYERS; j++) bfree(pk.presets[k].layers[j].params); bfree(pk.presets); return false; }
		pk.npresets++; obs_data_release(pd);
	}
	obs_data_array_release(ps); obs_data_release(m);
	/* licence */
	pk.licence.state = FF_LIC_NONE;
	if (pk.licensed) {
		struct dstr lp = {0}; dstr_printf(&lp, "%s/licence.json", dir);
		char *txt = os_quick_read_utf8_file(lp.array); dstr_free(&lp);
		if (!txt) { pk.licence.state = FF_LIC_INVALID; snprintf(pk.licence.reason, sizeof pk.licence.reason, "missing"); }
		else { ff_licence_verify(txt, strlen(txt), FF_PUBLIC_KEY, now, &pk.licence); bfree(txt); }
		struct dstr np = {0}; dstr_printf(&np, "%s/licensee.json", dir);
		obs_data_t *lz = obs_data_create_from_json_file(np.array); dstr_free(&np);
		if (lz) { snprintf(pk.licensee_name, sizeof pk.licensee_name, "%s", obs_data_get_string(lz, "display_name")); obs_data_release(lz); }
	}
	l->packs = brealloc(l->packs, sizeof(struct ff_pack) * (l->n + 1));
	l->packs[l->n++] = pk;
	return true;
}

static void scan_dir(struct ff_pack_list *l, const char *root, int64_t now)
{
	os_dir_t *d = os_opendir(root); if (!d) return;
	struct os_dirent *e;
	while ((e = os_readdir(d))) {
		if (!e->directory || e->d_name[0] == '.') continue;
		struct dstr p = {0}; dstr_printf(&p, "%s/%s", root, e->d_name);
		load_pack_dir(l, p.array, now); dstr_free(&p);
	}
	os_closedir(d);
}

static bool pubkey_is_zero(void) { for (int i = 0; i < 32; i++) if (FF_PUBLIC_KEY[i]) return false; return true; }

void ff_packs_scan(struct ff_pack_list *out, int64_t now)
{
	memset(out, 0, sizeof *out);
	static bool warned_pubkey = false;
	if (!warned_pubkey && pubkey_is_zero()) { obs_log(LOG_WARNING, "licence: public key not set; paid packs will not verify"); warned_pubkey = true; }
	char *bundled = obs_module_file("packs"); if (bundled) { scan_dir(out, bundled, now); bfree(bundled); }
	char *user = obs_module_config_path("packs"); if (user) { os_mkdirs(user); scan_dir(out, user, now); bfree(user); }
	obs_log(LOG_INFO, "packs: %zu loaded, %zu refused", out->n, out->nerrors);
}

void ff_packs_free(struct ff_pack_list *l)
{
	for (size_t i = 0; i < l->n; i++) { struct ff_pack *p = &l->packs[i]; for (size_t k = 0; k < p->npresets; k++) for (size_t j = 0; j < FF_MAX_LAYERS; j++) bfree(p->presets[k].layers[j].params); bfree(p->presets); }
	bfree(l->packs); memset(l, 0, sizeof *l);
}
const struct ff_pack *ff_packs_find(const struct ff_pack_list *l, const char *id) { for (size_t i = 0; i < l->n; i++) if (!strcmp(l->packs[i].id, id)) return &l->packs[i]; return NULL; }
const struct ff_preset *ff_pack_find_preset(const struct ff_pack *p, const char *id) { if (!p) return NULL; for (size_t i = 0; i < p->npresets; i++) if (!strcmp(p->presets[i].id, id)) return &p->presets[i]; return NULL; }

/* ---- zip install ---- */

static bool remove_recursive(const char *path)
{
	os_dir_t *d = os_opendir(path);
	if (!d) return os_rmdir(path) == 0 || !os_file_exists(path);
	struct os_dirent *e; bool ok = true;
	while ((e = os_readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		struct dstr p = {0}; dstr_printf(&p, "%s/%s", path, e->d_name);
		if (e->directory) { if (!remove_recursive(p.array)) ok = false; }
		else if (os_unlink(p.array) != 0) ok = false;
		dstr_free(&p);
	}
	os_closedir(d);
	if (os_rmdir(path) != 0) ok = false;
	return ok;
}

static bool run_and_capture(const char *cmd, struct dstr *out)
{
	dstr_free(out);
	os_process_pipe_t *pp = os_process_pipe_create(cmd, "r");
	if (!pp) return false;
	uint8_t buf[4096]; size_t n;
	while ((n = os_process_pipe_read(pp, buf, sizeof buf)) > 0) dstr_ncat(out, (const char *)buf, n);
	int rc = os_process_pipe_destroy(pp);
	return rc == 0;
}

static bool topdir_ok(const char *s, size_t len)
{
	if (len < 2 || s[len - 1] != '/') return false;
	for (size_t i = 0; i < len - 1; i++) { char c = s[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false; }
	return true;
}

/* first_line_and_check: the first listed entry must be a single [a-z0-9-]+/ top-level folder, and
   every other non-empty entry must live under it -- so an installed pack can never write outside
   packs/<id>/ */
static bool first_line_and_check(const struct dstr *listing, struct dstr *topdir, char *msg, size_t cap)
{
	const char *p = listing->array, *end = p + listing->len;
	while (p < end && (*p == '\n' || *p == '\r')) p++;
	const char *nl = memchr(p, '\n', (size_t)(end - p));
	size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
	while (linelen && p[linelen - 1] == '\r') linelen--;
	if (!topdir_ok(p, linelen)) { snprintf(msg, cap, "zip must contain a single top-level folder named [a-z0-9-]+"); return false; }
	dstr_ncopy(topdir, p, linelen);
	const char *q = listing->array;
	while (q < end) {
		const char *lend = memchr(q, '\n', (size_t)(end - q));
		size_t ll = lend ? (size_t)(lend - q) : (size_t)(end - q);
		while (ll && q[ll - 1] == '\r') ll--;
		if (ll > 0 && (ll < topdir->len || strncmp(q, topdir->array, topdir->len) != 0)) { snprintf(msg, cap, "zip contains an entry outside its top-level folder"); return false; }
		if (!lend) break;
		q = lend + 1;
	}
	return true;
}

bool ff_packs_install_zip(const char *zip_path, char *msg, size_t cap)
{
	msg[0] = 0;
	if (!zip_path || !os_file_exists(zip_path)) { snprintf(msg, cap, "zip file not found"); obs_log(LOG_WARNING, "pack install: zip file not found: %s", zip_path ? zip_path : "(null)"); return false; }

	char *packs_dir = obs_module_config_path("packs");
	if (!packs_dir) { snprintf(msg, cap, "could not resolve the packs directory"); obs_log(LOG_WARNING, "pack install: could not resolve the packs directory"); return false; }
	os_mkdirs(packs_dir);

	struct dstr list_cmd = {0};
#ifdef _WIN32
	dstr_printf(&list_cmd, "tar -tf \"%s\"", zip_path);
#else
	dstr_printf(&list_cmd, "unzip -Z1 \"%s\"", zip_path);
#endif
	struct dstr listing = {0};
	bool listed = run_and_capture(list_cmd.array, &listing) && listing.len > 0;
	dstr_free(&list_cmd);
	if (!listed) {
		snprintf(msg, cap, "could not read the zip's contents");
		obs_log(LOG_WARNING, "pack install: could not list zip contents: %s", zip_path);
		dstr_free(&listing); bfree(packs_dir);
		return false;
	}

	struct dstr topdir = {0};
	bool topdir_valid = first_line_and_check(&listing, &topdir, msg, cap);
	dstr_free(&listing);
	if (!topdir_valid) {
		obs_log(LOG_WARNING, "pack install: %s (%s)", msg, zip_path);
		dstr_free(&topdir); bfree(packs_dir);
		return false;
	}

	struct dstr tmp = {0}; dstr_printf(&tmp, "%s/.tmp-%d", packs_dir, (int)ff_getpid());
	remove_recursive(tmp.array); /* clear any stale leftover from a crashed prior install */
	os_mkdirs(tmp.array);

	struct dstr extract_cmd = {0};
#ifdef _WIN32
	dstr_printf(&extract_cmd, "tar -xf \"%s\" -C \"%s\"", zip_path, tmp.array);
#else
	dstr_printf(&extract_cmd, "unzip -o -d \"%s\" \"%s\"", tmp.array, zip_path);
#endif
	struct dstr extract_out = {0};
	bool extracted = run_and_capture(extract_cmd.array, &extract_out);
	dstr_free(&extract_cmd); dstr_free(&extract_out);
	if (!extracted) {
		snprintf(msg, cap, "failed to extract the zip");
		obs_log(LOG_WARNING, "pack install: failed to extract %s", zip_path);
		remove_recursive(tmp.array); dstr_free(&tmp); dstr_free(&topdir); bfree(packs_dir);
		return false;
	}

	struct dstr pack_src = {0};
	dstr_printf(&pack_src, "%s/%.*s", tmp.array, (int)(topdir.len ? topdir.len - 1 : 0), topdir.array);
	dstr_free(&topdir);

	struct ff_pack_list check; memset(&check, 0, sizeof check);
	bool ok = load_pack_dir(&check, pack_src.array, (int64_t)time(NULL));
	if (!ok) {
		snprintf(msg, cap, "%s", check.nerrors ? check.errors[0] : "pack failed validation");
		ff_packs_free(&check);
		remove_recursive(tmp.array); dstr_free(&tmp); dstr_free(&pack_src); bfree(packs_dir);
		return false;
	}

	char id[64]; snprintf(id, sizeof id, "%s", check.packs[0].id);
	ff_packs_free(&check);

	struct dstr dest = {0}; dstr_printf(&dest, "%s/%s", packs_dir, id);
	remove_recursive(dest.array); /* replace an existing pack of the same id */
	bool renamed = os_rename(pack_src.array, dest.array) == 0;
	dstr_free(&pack_src); dstr_free(&dest);
	remove_recursive(tmp.array); dstr_free(&tmp);
	bfree(packs_dir);
	if (!renamed) {
		snprintf(msg, cap, "failed to install pack '%s' into place", id);
		obs_log(LOG_WARNING, "pack install: rename failed for '%s'", id);
		return false;
	}
	obs_log(LOG_INFO, "pack install: installed '%s'", id);
	return true;
}
