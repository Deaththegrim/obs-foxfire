#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include "ff-frame.h"
#define FF_HANDOFF_MAX_TRIES 1024
struct ff_handoff {
	struct ff_frame slot[2];
	atomic_uint seq;
};
void ff_handoff_init(struct ff_handoff *h);
void ff_handoff_publish(struct ff_handoff *h, const struct ff_frame *f);
/* ff_handoff_read: returns false when nothing has been published yet or the reader lost the race FF_HANDOFF_MAX_TRIES times;
   callers keep their previous frame */
bool ff_handoff_read(struct ff_handoff *h, struct ff_frame *out);
/* The publish counter. Monotonic, two per publish (the seqlock bumps it either side), so a reader
   that sees the SAME value twice knows no new frame arrived between the two reads -- which is the
   only way to tell "this source is being fed silence" from "nothing is feeding this source at
   all". Both look identical in the frame itself: all bands zero. */
unsigned ff_handoff_seq(const struct ff_handoff *h);
