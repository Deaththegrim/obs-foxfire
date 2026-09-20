#include "ff-audio.h"
#include "ff-handoff.h"
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/threading.h>
#include <string.h>
#include <plugin-support.h>

struct ff_audio {
	struct ff_analysis *an;
	struct ff_handoff ho;
	pthread_mutex_t lock;            /* guards an/params against configure() vs callback */
	enum ff_audio_mode mode;
	char source_name[256];
	obs_weak_source_t *weak;         /* followed source */
	bool master_connected;
	float mono[4096];
	struct ff_frame scratch;
};

static void feed(struct ff_audio *a, const float *l, const float *r, uint32_t frames)
{
	while (frames) {
		uint32_t n = frames > 4096 ? 4096 : frames;
		for (uint32_t i = 0; i < n; i++) a->mono[i] = r ? 0.5f * (l[i] + r[i]) : l[i];
		pthread_mutex_lock(&a->lock);
		bool upd = ff_analysis_push(a->an, a->mono, n, &a->scratch);
		pthread_mutex_unlock(&a->lock);
		if (upd) ff_handoff_publish(&a->ho, &a->scratch);
		l += n; if (r) r += n; frames -= n;
	}
}

static void master_cb(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct ff_audio *a = param;
	feed(a, (const float *)data->data[0], data->data[1] ? (const float *)data->data[1] : NULL, data->frames);
}

static void source_cb(void *param, obs_source_t *src, const struct audio_data *data, bool muted)
{
	UNUSED_PARAMETER(src);
	struct ff_audio *a = param;
	if (muted) { static float z[1024]; uint32_t left = data->frames; while (left) { uint32_t n = left > 1024 ? 1024 : left; feed(a, z, NULL, n); left -= n; } return; }
	feed(a, (const float *)data->data[0], data->data[1] ? (const float *)data->data[1] : NULL, data->frames);
}

static void disconnect_all(struct ff_audio *a)
{
	if (a->master_connected) { audio_output_disconnect(obs_get_audio(), 0, master_cb, a); a->master_connected = false; }
	if (a->weak) {
		obs_source_t *s = obs_weak_source_get_source(a->weak);
		if (s) { obs_source_remove_audio_capture_callback(s, source_cb, a); obs_source_release(s); }
		obs_weak_source_release(a->weak); a->weak = NULL;
	}
}

static void connect_master(struct ff_audio *a)
{
	struct audio_convert_info conv = { .samples_per_sec = 48000, .format = AUDIO_FORMAT_FLOAT_PLANAR, .speakers = SPEAKERS_STEREO };
	a->master_connected = audio_output_connect(obs_get_audio(), 0, &conv, master_cb, a);
	if (!a->master_connected) obs_log(LOG_WARNING, "audio: could not connect to the master mix");
}

static bool connect_source(struct ff_audio *a)
{
	obs_source_t *s = obs_get_source_by_name(a->source_name);
	if (!s) return false;
	if (!(obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO)) { obs_source_release(s); return false; }
	a->weak = obs_source_get_weak_source(s);
	obs_source_add_audio_capture_callback(s, source_cb, a);
	obs_source_release(s);
	return true;
}

/* re-resolve a followed source when it appears/renames */
static void on_source_signal(void *param, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct ff_audio *a = param;
	if (a->mode != FF_AUDIO_SOURCE || a->weak) return;
	connect_source(a);
}

struct ff_audio *ff_audio_create(void)
{
	struct ff_audio *a = bzalloc(sizeof *a);
	a->an = ff_analysis_create(48000);
	ff_handoff_init(&a->ho);
	pthread_mutex_init(&a->lock, NULL);
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_create", on_source_signal, a);
	signal_handler_connect(sh, "source_rename", on_source_signal, a);
	return a;
}

void ff_audio_destroy(struct ff_audio *a)
{
	if (!a) return;
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_create", on_source_signal, a);
	signal_handler_disconnect(sh, "source_rename", on_source_signal, a);
	disconnect_all(a);
	pthread_mutex_destroy(&a->lock);
	ff_analysis_destroy(a->an);
	bfree(a);
}

void ff_audio_configure(struct ff_audio *a, enum ff_audio_mode mode, const char *source_name, const struct ff_analysis_params *p)
{
	pthread_mutex_lock(&a->lock); ff_analysis_set_params(a->an, p); pthread_mutex_unlock(&a->lock);
	bool same = a->mode == mode && (mode == FF_AUDIO_MASTER || (source_name && strcmp(a->source_name, source_name) == 0));
	if (same && (a->master_connected || a->weak)) return;
	disconnect_all(a);
	a->mode = mode;
	if (mode == FF_AUDIO_MASTER) { connect_master(a); return; }
	snprintf(a->source_name, sizeof a->source_name, "%s", source_name ? source_name : "");
	if (!connect_source(a)) obs_log(LOG_WARNING, "audio: source '%s' not found or has no audio; rendering silence until it appears", a->source_name);
}

bool ff_audio_read(struct ff_audio *a, struct ff_frame *out) { return ff_handoff_read(&a->ho, out); }

bool ff_audio_status(struct ff_audio *a, char *msg, size_t cap)
{
	if (a->mode == FF_AUDIO_SOURCE) {
		obs_source_t *s = a->weak ? obs_weak_source_get_source(a->weak) : NULL;
		if (!s) { snprintf(msg, cap, "Audio source '%s' not found; showing silence.", a->source_name); return false; }
		obs_source_release(s);
	}
	msg[0] = 0; return true;
}
