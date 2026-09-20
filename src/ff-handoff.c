#include "ff-handoff.h"
#include <string.h>

void ff_handoff_init(struct ff_handoff *h) { memset(h, 0, sizeof *h); atomic_store(&h->seq, 0); }

void ff_handoff_publish(struct ff_handoff *h, const struct ff_frame *f)
{
	unsigned s = atomic_load_explicit(&h->seq, memory_order_relaxed);
	atomic_store_explicit(&h->seq, s + 1, memory_order_release);   /* odd: writing */
	h->slot[((s + 2) / 2) & 1] = *f;
	atomic_store_explicit(&h->seq, s + 2, memory_order_release);   /* even: slot (s+2)/2 &1 is the newest */
}

bool ff_handoff_read(struct ff_handoff *h, struct ff_frame *out)
{
	for (int tries = 0; tries < 1024; tries++) {
		unsigned s = atomic_load_explicit(&h->seq, memory_order_acquire);
		if (s == 0) return false;
		if (s & 1u) continue;                     /* writer mid-flight */
		*out = h->slot[(s / 2) & 1];
		atomic_thread_fence(memory_order_acquire);
		unsigned s2 = atomic_load_explicit(&h->seq, memory_order_relaxed);
		if (s2 - s < 3u) return true;   /* our slot untouched since the copy */
	}
	return false; /* reader lost the race 256 times; caller keeps previous frame */
}
