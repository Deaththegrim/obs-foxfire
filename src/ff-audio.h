#pragma once
#include "ff-analysis.h"
#include "ff-frame.h"

enum ff_audio_mode { FF_AUDIO_MASTER = 0, FF_AUDIO_SOURCE = 1 };
struct ff_audio; /* opaque; owns the analysis and the handoff */
struct ff_audio *ff_audio_create(void);
/* Must not race ff_audio_configure / ff_audio_read / ff_audio_status: it tears down the locks
   those take. The owning ff_instance is the only holder of the pointer, and libobs serialises a
   source's update/destroy callbacks against each other, so the ordering holds by construction. */
void ff_audio_destroy(struct ff_audio *a);
/* (re)configure; source_name ignored for MASTER; safe to call from update() */
void ff_audio_configure(struct ff_audio *a, enum ff_audio_mode mode, const char *source_name,
			const struct ff_analysis_params *p);
bool ff_audio_read(struct ff_audio *a, struct ff_frame *out); /* render thread */
/* status for the properties: returns false and fills msg when following a source that is missing */
bool ff_audio_status(struct ff_audio *a, char *msg, size_t cap);
