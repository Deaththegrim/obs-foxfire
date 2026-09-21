#pragma once
#include <stdint.h>
#define FF_BANDS 64
#define FF_WAVE 512
#define FF_FFT 2048
/* A second, longer window used ONLY below FF_XOVER_HZ. At 48 kHz a 2048-point FFT has 23.44 Hz
   bins, which is wider than the log bands ask for down low: bands 0-4 all collapse onto bin 1,
   5-8 onto bin 2, and so on, so 18 of the 64 bars were exact duplicates of a neighbour and moved
   as one block. 8192 points gives 5.86 Hz bins, which resolves them. The cost is that the bass
   reflects a 171 ms window instead of 43 ms; that is acceptable precisely because bass moves
   slowly, and the fast window still drives mids, highs, level, peak and beat detection. */
#define FF_FFT_LOW 8192
#define FF_RING FF_FFT_LOW /* one ring, long enough for the longer of the two windows */
#define FF_HOP 1024
struct ff_frame {
	float bands[FF_BANDS];
	float peaks[FF_BANDS];
	float wave[FF_WAVE];
	float level, peak, bass, mid, treble, beat;
	uint32_t beat_count;
};
