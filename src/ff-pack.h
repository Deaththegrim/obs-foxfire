#pragma once
#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>
#include "ff-licence.h"

#define FF_MAX_LAYERS 8
#define FF_MAX_PRESETS 64

struct ff_param_override { char name[64]; float v[4]; int is_color; }; /* preset "params" entries */
struct ff_layer_def { char effect_path[512]; struct ff_param_override *params; size_t nparams; };
struct ff_preset { char id[64], name[128], kind[16], thumb[512]; bool heavy; struct ff_layer_def layers[FF_MAX_LAYERS]; size_t nlayers; };
struct ff_pack {
	char id[64], name[128], version[32], dir[1024];
	bool licensed;
	struct ff_preset *presets; size_t npresets;
	struct ff_licence licence;   /* state NONE when unlicensed pack */
	char licensee_name[128];     /* from licensee.json display_name, may be empty */
};
/* errors[] entries are 512 bytes (not 256): a refusal reason plus the pack directory's basename
   must fit without a second truncation on top of the (already-capped) reason text -- see add_error() */
struct ff_pack_list { struct ff_pack *packs; size_t n; char errors[8][512]; size_t nerrors; };

/* scans data/packs/* (bundled) then obs_module_config_path("packs")/*; bad packs are skipped and named in errors */
void ff_packs_scan(struct ff_pack_list *out, int64_t now);
void ff_packs_free(struct ff_pack_list *l);
const struct ff_pack *ff_packs_find(const struct ff_pack_list *l, const char *id);
const struct ff_preset *ff_pack_find_preset(const struct ff_pack *p, const char *id);
/* validates + extracts a .zip into the config packs dir; returns false with msg */
bool ff_packs_install_zip(const char *zip_path, char *msg, size_t cap);

extern const uint8_t FF_PUBLIC_KEY[32];
