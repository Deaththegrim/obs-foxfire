#pragma once
#include <obs-module.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How the engine describes text to whichever text source this OBS happens to have. A pack writes
   THESE fields, never a source's own settings keys -- see ff-alert.c's header for why. */
struct ff_alert_text {
	char body[512];
	char face[128];
	int size;
	uint32_t colour; /* 0xAABBGGRR, the same packing OBS colour pickers use */
	bool bold, outline, shadow;
};

/* The first text source kind this OBS registers, or NULL if it registers none we can drive.
   Logged once, either way. Safe to call from any thread after modules have loaded. */
const char *ff_alert_text_kind(void);

/* Pushes `t` into an existing text source of whichever kind ff_alert_text_kind() found. */
void ff_alert_text_apply(obs_source_t *text, const struct ff_alert_text *t);

/* Copies `in` into `out` with control characters and bidirectional overrides removed and the
   result truncated to fit `cap` WITHOUT splitting a UTF-8 sequence. Returns how many characters
   were removed, for logging. A username is hostile input painted on a live stream. */
size_t ff_alert_sanitise(const char *in, char *out, size_t cap);

/* Fires an alert on a foxfire_alert source: sanitises `raw_name`, substitutes it into the
   source's template, pushes the result into the text child, restarts the sound and starts the
   clock. `data` is the source's private data. Safe to call from any thread that OBS calls a
   source callback on. */
void ff_alert_fire(void *data, const char *raw_name);
