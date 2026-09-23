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
/* Reads the newest analysis frame into the CALLER'S buffer. Safe from any thread and from several
   at once: ff_handoff is a single-writer seqlock whose reader mutates nothing in the struct and
   whose writer never waits, so the render thread and a UI-side meter poll it independently without
   contending. A lost race returns false and the caller keeps its previous frame.
   (This said "render thread" while that was the only caller; the dock's per-source proc handler in
   ff-props.c is the second, and the comment would otherwise read as a prohibition it never was.) */
bool ff_audio_read(struct ff_audio *a, struct ff_frame *out);
/* Everything a panel needs to say about where this instance's audio comes from, under one taking
   of conn_lock. Any out-parameter may be NULL / zero-capacity. `name` is empty in MASTER mode
   rather than the stale last-followed source. Returns false, and fills msg, when following a
   source that is missing. */
bool ff_audio_describe(struct ff_audio *a, enum ff_audio_mode *mode, char *name, size_t name_cap, char *msg,
		       size_t msg_cap);
/* status for the properties: returns false and fills msg when following a source that is missing */
bool ff_audio_status(struct ff_audio *a, char *msg, size_t cap);

/* How many frames this instance has ever published, for staleness. Takes no lock -- it is one
   atomic load through the handoff, the same way ff_audio_read goes. */
unsigned ff_audio_seq(struct ff_audio *a);
