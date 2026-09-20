#pragma once
#include <obs-module.h>
#include <graphics/graphics.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "ff-audio.h"
#include "ff-frame.h"
#include "ff-layers.h"
#include "ff-pack.h"

/* Settings keys shared by the source (ff-source.c) and the filter (ff-filter.c).
   The renderer owns three more prefixes -- "l<idx>.", "grp." and "lerr." -- so nothing here may
   start with an "l" followed by a digit: erase_layer_keys() wipes exactly that shape on a preset
   switch and would take a key of ours with it. */
#define S_PACK "pack"             /* string id */
#define S_PRESET "preset"         /* string id */
#define S_WIDTH "width"           /* int, source only, default 1920 */
#define S_HEIGHT "height"         /* int, source only, default 1080 */
#define S_AUDIO_MODE "audio_mode" /* int: 0 master, 1 source */
#define S_AUDIO_SOURCE "audio_source"
#define S_GAIN "gain_db"          /* float -24..24, default 0 */
#define S_RELEASE "release_ms"    /* float 20..2000, default 150 */
#define S_BEAT "beat_sensitivity" /* float 0.1..3, default 1 */
#define S_INSTALL "install_zip"   /* path */

/* See the threading contract at the top of ff-props.c: state_lock is what lets the UI thread read
   this while the video thread rewrites it. */
struct ff_instance {
	pthread_mutex_t state_lock;
	obs_source_t *self;
	bool is_filter;
	struct ff_audio *audio;
	struct ff_renderer *renderer;
	struct ff_pack_list packs;
	char pack_id[64], preset_id[64];
	uint32_t width, height;
	float dt;                /* video thread only -- see the threading contract in ff-props.c. bzalloc leaves this
		     0 for an instance's first rendered frame (tick hasn't run yet the very first
		     time create() is followed by a render before any tick); harmless, since the only
		     use is r->time += dt in ff_renderer_render, so one 0-length frame of animation
		     time just doesn't advance -- nothing divides by it. */
	char status[256];        /* last refusal sentence for the current pack/preset, empty when fine */
	char install_msg[256];   /* last "Install pack" result; update() must not clear it, see below */
	gs_texrender_t *capture; /* filter only: the target, captured premultiplied -- see ff-filter.c */
	struct ff_frame frame;   /* last frame read from the audio tap; kept when none arrived */
	bool install_failed;     /* colours the install line: a refusal must not read as a success */
	bool reload_pending;     /* the Reload button asks for a re-read without a preset switch */
	bool initialised;        /* false until the first update(): there is no previous preset yet */
};

struct ff_instance *ff_instance_create(obs_data_t *settings, obs_source_t *self, bool is_filter);
void ff_instance_destroy(struct ff_instance *in);
void ff_instance_update(struct ff_instance *in, obs_data_t *settings);
obs_properties_t *ff_instance_properties(struct ff_instance *in);
void ff_instance_defaults(obs_data_t *settings, bool is_filter);
/* per-frame: read audio, render stack; returns final texture (NULL when nothing to draw).
   Uses in->dt, written by the caller's video_tick. Graphics context required -- call it from
   video_render only. */
gs_texture_t *ff_instance_render(struct ff_instance *in, gs_texture_t *input, uint32_t w, uint32_t h);
