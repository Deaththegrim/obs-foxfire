#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ff-frame.h"
struct ff_analysis;
struct ff_analysis_params { float release_ms; float beat_sensitivity; float gain_db; };
struct ff_analysis *ff_analysis_create(uint32_t sample_rate);
void ff_analysis_destroy(struct ff_analysis *a);
void ff_analysis_set_params(struct ff_analysis *a, const struct ff_analysis_params *p);
bool ff_analysis_push(struct ff_analysis *a, const float *mono, size_t frames, struct ff_frame *out);
