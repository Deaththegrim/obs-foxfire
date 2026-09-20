#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include "ff-frame.h"
struct ff_handoff { struct ff_frame slot[2]; atomic_uint seq; };
void ff_handoff_init(struct ff_handoff *h);
void ff_handoff_publish(struct ff_handoff *h, const struct ff_frame *f);
bool ff_handoff_read(struct ff_handoff *h, struct ff_frame *out);
