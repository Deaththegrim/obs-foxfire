#pragma once
#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>
#include "ff-licence.h"

#define FF_MAX_LAYERS 8
#define FF_MAX_PRESETS 64

struct ff_param_override {
	char name[64];
	float v[4];
	int is_color;
	char str[512]; /* a STRING value, empty when the entry was not one. Only meaningful for a
	                  texture param, where it selects which of the pack's images this preset
	                  uses -- so one shader can serve a whole pack of art instead of being
	                  copied once per image. */
	int is_str;
}; /* preset "params" entries */
struct ff_layer_def {
	char effect_path[512];
	struct ff_param_override *params;
	size_t nparams;
};
struct ff_preset {
	char id[64], name[128], kind[16], thumb[512];
	bool heavy;
	struct ff_layer_def layers[FF_MAX_LAYERS];
	size_t nlayers;
};
struct ff_pack {
	char id[64], name[128], version[32], dir[1024];
	bool licensed;
	int64_t released; /* unix seconds from the manifest's "released"; 0 when absent, which is
	                     before every entitlement and so always unlocks -- a pack authored before
	                     this field existed must not break */
	struct ff_preset *presets;
	size_t npresets;
	struct ff_licence licence; /* state NONE when unlicensed pack */
	char licensee_name[128];   /* from licensee.json display_name, may be empty */
};
/* errors[] entries are 512 bytes (not 256): a refusal reason plus the pack directory's basename
   must fit without a second truncation on top of the (already-capped) reason text -- see add_error() */
struct ff_pack_list {
	struct ff_pack *packs;
	size_t n;
	char errors[8][512];
	size_t nerrors;
};

/* scans data/packs/* (bundled) then obs_module_config_path("packs")/*; bad packs are skipped and named in errors */
/* No clock argument, deliberately. Entitlement compares the pack's release date against the
   licence's, so scanning does not depend on what time it is and cannot change its answer while
   OBS is running. */
void ff_packs_scan(struct ff_pack_list *out);
void ff_packs_free(struct ff_pack_list *l);
const struct ff_pack *ff_packs_find(const struct ff_pack_list *l, const char *id);
const struct ff_preset *ff_pack_find_preset(const struct ff_pack *p, const char *id);
/* validates + extracts a .zip into the config packs dir; returns false with msg */
bool ff_packs_install_zip(const char *zip_path, char *msg, size_t cap);

/* true when a path is safe to join to a pack dir: relative, no "..", no backslash, no colon.
   Callers must ALSO confirm the file exists inside the pack -- this judges the name, not the inode. */
bool ff_rel_ok(const char *p);

/* Rewrites a per-module config path into the one every Foxfire plugin shares:
   ".../plugin_config/<this plugin>/<anything>" becomes ".../plugin_config/foxfire/<leaf>".
   Returns false WITHOUT writing `out` when the "plugin_config" component is not there, so the
   caller can fall back to its own directory and say so rather than reading a wrong path
   silently. Exported for unit testing; the surgery is pure string work and needs no OBS. */
bool ff_shared_config_path(const char *module_path, const char *leaf, char *out, size_t cap);

/* Loads ONE pack directory (its pack.json, presets and licence) and appends it to `l`, or
   appends a refusal reason to l->errors and returns false. Exported for unit testing: the
   manifest refusals are the gate on shipping a paid pack, and reaching them through
   ff_packs_scan() would mean writing into the module's own data or config directory.
   Does not call obs_current_module(). */
bool ff_pack_load_dir(struct ff_pack_list *l, const char *dir);

/* recursively remove a directory, including symlinks and their targets (does not follow links).
   Exported for unit testing; returns false on any failure, and logs warnings for each failure. */
bool ff_remove_recursive(const char *path);

extern const uint8_t FF_PUBLIC_KEY[32];
