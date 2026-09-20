#pragma once
#include <stdint.h>
#define FF_BANDS 64
#define FF_WAVE 512
#define FF_FFT 2048
#define FF_HOP 1024
struct ff_frame {
	float bands[FF_BANDS];
	float peaks[FF_BANDS];
	float wave[FF_WAVE];
	float level, peak, bass, mid, treble, beat;
	uint32_t beat_count;
};
