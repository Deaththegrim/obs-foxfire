#include "ff-test.h"
#include "ff-analysis.h"
#include <string.h>

static void tone(float *buf, size_t n, float hz, float sr, float amp, size_t *phase)
{
	for (size_t i = 0; i < n; i++, (*phase)++)
		buf[i] = amp * sinf(2.0f * 3.14159265f * hz * (float)(*phase) / sr);
}

int main(void)
{
	const float sr = 48000.f;
	struct ff_analysis *a = ff_analysis_create(48000);
	struct ff_analysis_params p = {.release_ms = 150.f, .beat_sensitivity = 1.0f, .gain_db = 0.f};
	ff_analysis_set_params(a, &p);
	struct ff_frame f;
	memset(&f, 0, sizeof f);
	float buf[FF_HOP];
	size_t ph = 0;

	/* silence → everything zero, no beats */
	memset(buf, 0, sizeof buf);
	for (int i = 0; i < 20; i++)
		ff_analysis_push(a, buf, FF_HOP, &f);
	CHECK(f.level == 0.f && f.peak == 0.f && f.beat == 0.f && f.beat_count == 0);
	for (int b = 0; b < FF_BANDS; b++)
		CHECK(f.bands[b] == 0.f);

	/* 100 Hz tone → energy in the low bands (band ≈ 64·ln(100/30)/ln(16000/30) ≈ 12), bass > treble, level > 0 */
	for (int i = 0; i < 40; i++) {
		tone(buf, FF_HOP, 100.f, sr, 0.5f, &ph);
		ff_analysis_push(a, buf, FF_HOP, &f);
	}
	float low = 0, high = 0;
	for (int b = 0; b < 16; b++)
		low += f.bands[b]; /* 30..~180 Hz: a 100 Hz tone lands near band 12 */
	for (int b = 40; b < FF_BANDS; b++)
		high += f.bands[b];
	CHECK(low > 0.5f);
	CHECK(high < 0.2f * low);
	CHECK(f.bass > f.treble);
	CHECK_NEAR(f.level, 0.5f / sqrtf(2.f), 0.08f); /* RMS of a 0.5 amplitude sine */
	CHECK(f.peak > 0.4f && f.peak <= 1.0f);

	/* waveform carries the last samples: values within -1..1 and not all zero */
	int nonzero = 0;
	for (int i = 0; i < FF_WAVE; i++) {
		CHECK(f.wave[i] >= -1.f && f.wave[i] <= 1.f);
		if (f.wave[i] != 0.f)
			nonzero++;
	}
	CHECK(nonzero > FF_WAVE / 2);

	/* peak-hold with gravity: band 12 carries the 100 Hz tone */
	float p0 = f.peaks[12];
	CHECK(p0 >= f.bands[12] - 1e-4f);

	/* release: after silence the bands (and peaks) decay but not instantly */
	memset(buf, 0, sizeof buf);
	ff_analysis_push(a, buf, FF_HOP, &f);
	CHECK(low > 0.f && f.bands[2] > 0.f && f.bands[2] < 1.f);
	for (int i = 0; i < 4; i++)
		ff_analysis_push(a, buf, FF_HOP, &f); /* 5 hops of silence total */
	CHECK(f.peaks[12] > 0.f && f.peaks[12] < p0 && f.peaks[12] >= f.bands[12]);
	for (int i = 0; i < 96; i++)
		ff_analysis_push(a, buf, FF_HOP, &f); /* 101 hops of silence total */
	CHECK(f.bands[2] < 0.01f);
	for (int i = 0; i < 99; i++)
		ff_analysis_push(a, buf, FF_HOP, &f); /* 200 hops of silence total */
	CHECK(f.peaks[12] == 0.f);

	/* click train at 2 Hz for 5 s → 10 ± 1 beats, beat envelope decays */
	ff_analysis_destroy(a);
	a = ff_analysis_create(48000);
	ff_analysis_set_params(a, &p);
	uint32_t before = 0;
	size_t sample = 0;
	const size_t period = 24000;
	for (int hop = 0; hop < (5 * 48000) / FF_HOP; hop++) {
		for (size_t i = 0; i < FF_HOP; i++, sample++)
			buf[i] = (sample % period) < 96 ? 0.9f : 0.f;
		ff_analysis_push(a, buf, FF_HOP, &f);
	}
	CHECK(f.beat_count >= 9 && f.beat_count <= 11);

	/* keep clicking the same 2 Hz stream until a fresh onset leaves f.beat > 0.5, bounded */
	int guard = 0;
	while (f.beat <= 0.5f && guard < 300) {
		for (size_t i = 0; i < FF_HOP; i++, sample++)
			buf[i] = (sample % period) < 96 ? 0.9f : 0.f;
		ff_analysis_push(a, buf, FF_HOP, &f);
		guard++;
	}
	CHECK(f.beat > 0.5f);
	before = f.beat_count;
	float b0 = f.beat;

	/* strict decay: the beat envelope drops meaningfully but not instantly across 3 silent hops */
	memset(buf, 0, sizeof buf);
	for (int i = 0; i < 3; i++)
		ff_analysis_push(a, buf, FF_HOP, &f);
	CHECK(f.beat < b0 * 0.9f);
	CHECK(f.beat > 0.f);
	CHECK(f.beat_count == before);
	for (int i = 0; i < 57; i++)
		ff_analysis_push(a, buf, FF_HOP, &f); /* 60 silent hops total since b0 */
	CHECK(f.beat == 0.f);

	/* refractory: two clicks ~43 ms apart (inside the ~107 ms window) count once */
	ff_analysis_destroy(a);
	a = ff_analysis_create(48000);
	ff_analysis_set_params(a, &p);
	{
		float stream[4 * FF_HOP];
		memset(stream, 0, sizeof stream);
		for (size_t i = 0; i < 96; i++)
			stream[i] = 0.9f;
		for (size_t i = 0; i < 96; i++)
			stream[2048 + i] = 0.9f;
		for (int hop = 0; hop < 4; hop++) {
			memcpy(buf, stream + (size_t)hop * FF_HOP, FF_HOP * sizeof(float));
			ff_analysis_push(a, buf, FF_HOP, &f);
		}
		memset(buf, 0, sizeof buf);
		for (int i = 0; i < 20; i++)
			ff_analysis_push(a, buf, FF_HOP, &f);
		CHECK(f.beat_count == 1);
	}

	/* refractory: clicks 200 ms apart (well outside the window) each count */
	ff_analysis_destroy(a);
	a = ff_analysis_create(48000);
	ff_analysis_set_params(a, &p);
	{
		const size_t starts[5] = {0, 9600, 19200, 28800, 38400};
		size_t s = 0;
		int hops = (int)((starts[4] + 96) / FF_HOP) + 15; /* past the last click, plenty of decay room */
		for (int hop = 0; hop < hops; hop++) {
			for (size_t i = 0; i < FF_HOP; i++, s++) {
				int hot = 0;
				for (int c = 0; c < 5; c++)
					if (s >= starts[c] && s < starts[c] + 96) {
						hot = 1;
						break;
					}
				buf[i] = hot ? 0.9f : 0.f;
			}
			ff_analysis_push(a, buf, FF_HOP, &f);
		}
		CHECK(f.beat_count == 5);
	}

	/* band calibration: a 0 dBFS 1 kHz sine lights its band near 1.0, a -60 dBFS one near 0 */
	{
		int b1k = (int)floorf(64.f * logf(1000.f / 30.f) / logf(16000.f / 30.f));
		ff_analysis_destroy(a);
		a = ff_analysis_create(48000);
		ff_analysis_set_params(a, &p);
		ph = 0;
		for (int i = 0; i < 40; i++) {
			tone(buf, FF_HOP, 1000.f, sr, 1.0f, &ph);
			ff_analysis_push(a, buf, FF_HOP, &f);
		}
		float mx = 0.f;
		for (int b = b1k - 1; b <= b1k + 1; b++)
			if (b >= 0 && b < FF_BANDS && f.bands[b] > mx)
				mx = f.bands[b];
		CHECK(mx >= 0.95f);

		ff_analysis_destroy(a);
		a = ff_analysis_create(48000);
		ff_analysis_set_params(a, &p);
		ph = 0;
		for (int i = 0; i < 40; i++) {
			tone(buf, FF_HOP, 1000.f, sr, 0.001f, &ph);
			ff_analysis_push(a, buf, FF_HOP, &f);
		}
		mx = 0.f;
		for (int b = b1k - 1; b <= b1k + 1; b++)
			if (b >= 0 && b < FF_BANDS && f.bands[b] > mx)
				mx = f.bands[b];
		CHECK(mx <= 0.05f);
	}

	/* Low-band resolution: adjacent bass bands must not be bit-identical.
	   With a single 2048-point FFT they were: 23.44 Hz bins are wider than the log bands ask for
	   down low, so bands 0-4 all collapsed onto bin 1, 5-8 onto bin 2 and 9-11 onto bin 3 -- at
	   most THREE distinct values across bands 0..11, which on screen is three solid blocks.
	   The long-window FFT resolves them. Driven with five separate low tones so the region has
	   real structure to resolve rather than one peak and a skirt. */
	{
		ff_analysis_destroy(a);
		a = ff_analysis_create(48000);
		p.gain_db = 0.f;
		ff_analysis_set_params(a, &p);
		size_t lph = 0;
		for (int i = 0; i < 60; i++) {
			for (size_t k = 0; k < FF_HOP; k++, lph++) {
				float t = (float)lph / sr;
				buf[k] = 0.18f *
					 (sinf(2.f * 3.14159265f * 35.f * t) + sinf(2.f * 3.14159265f * 45.f * t) +
					  sinf(2.f * 3.14159265f * 60.f * t) + sinf(2.f * 3.14159265f * 80.f * t) +
					  sinf(2.f * 3.14159265f * 110.f * t));
			}
			ff_analysis_push(a, buf, FF_HOP, &f);
		}
		int distinct = 0;
		for (int i = 0; i < 12; i++) {
			int seen = 0;
			for (int j = 0; j < i; j++)
				if (f.bands[j] == f.bands[i])
					seen = 1;
			if (!seen)
				distinct++;
		}
		/* three is what the single-FFT build produced; anything at or below it means the long
		   window is not being consulted. Ten of twelve leaves room for two genuine ties. */
		CHECK(distinct >= 10);

		/* and the region has to actually be lit -- a silent build would trivially have one
		   distinct value, but a broken one that zeroed the bands would too */
		float lowsum = 0.f;
		for (int i = 0; i < 12; i++)
			lowsum += f.bands[i];
		CHECK(lowsum > 1.0f);
	}

	/* Crossover continuity: the two FFTs use different Hann coherent-gain normalisation (4/N),
	   so getting N wrong would put a visible step in the spectrum exactly at FF_XOVER_HZ.
	   A sine either side of it must read comparably.

	   The amplitude here is deliberately NOT full scale. db_to_unit clamps at 1.0, so a
	   full-scale probe reads ~1.0 on both sides even with the normalisation four times wrong --
	   the check would pass while blind to the very defect it names. At 0.05 the reading sits
	   near 0.57, mid-range, where a 4x error moves it to ~0.77 and is caught. */
	{
		float below = 0.f, above = 0.f;
		const float probes[2] = {200.f, 320.f};
		for (int side = 0; side < 2; side++) {
			ff_analysis_destroy(a);
			a = ff_analysis_create(48000);
			ff_analysis_set_params(a, &p);
			ph = 0;
			for (int i = 0; i < 60; i++) {
				tone(buf, FF_HOP, probes[side], sr, 0.05f, &ph);
				ff_analysis_push(a, buf, FF_HOP, &f);
			}
			float mx = 0.f;
			for (int bb = 0; bb < FF_BANDS; bb++)
				if (f.bands[bb] > mx)
					mx = f.bands[bb];
			if (side == 0)
				below = mx;
			else
				above = mx;
		}
		CHECK(below > 0.4f && below < 0.75f); /* mid-range, so a scaling error has room to show */
		CHECK(above > 0.4f && above < 0.75f);
		CHECK(fabsf(below - above) < 0.08f);
	}

	/* gain: +12 dB on a quiet tone raises level */
	ff_analysis_destroy(a);
	a = ff_analysis_create(48000);
	ff_analysis_set_params(a, &p);
	ph = 0;
	for (int i = 0; i < 40; i++) {
		tone(buf, FF_HOP, 440.f, sr, 0.05f, &ph);
		ff_analysis_push(a, buf, FF_HOP, &f);
	}
	float quiet = f.level;
	p.gain_db = 12.f;
	ff_analysis_set_params(a, &p);
	for (int i = 0; i < 40; i++) {
		tone(buf, FF_HOP, 440.f, sr, 0.05f, &ph);
		ff_analysis_push(a, buf, FF_HOP, &f);
	}
	CHECK(f.level > quiet * 3.0f);

	ff_analysis_destroy(a);
	FF_TEST_MAIN_END();
}
