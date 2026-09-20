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
	struct ff_analysis_params p = { .release_ms = 150.f, .beat_sensitivity = 1.0f, .gain_db = 0.f };
	ff_analysis_set_params(a, &p);
	struct ff_frame f; memset(&f, 0, sizeof f);
	float buf[FF_HOP];
	size_t ph = 0;

	/* silence → everything zero, no beats */
	memset(buf, 0, sizeof buf);
	for (int i = 0; i < 20; i++) ff_analysis_push(a, buf, FF_HOP, &f);
	CHECK(f.level == 0.f && f.peak == 0.f && f.beat == 0.f && f.beat_count == 0);
	for (int b = 0; b < FF_BANDS; b++) CHECK(f.bands[b] == 0.f);

	/* 100 Hz tone → energy in the low bands (band ≈ 64·ln(100/30)/ln(16000/30) ≈ 12), bass > treble, level > 0 */
	for (int i = 0; i < 40; i++) { tone(buf, FF_HOP, 100.f, sr, 0.5f, &ph); ff_analysis_push(a, buf, FF_HOP, &f); }
	float low = 0, high = 0;
	for (int b = 0; b < 16; b++) low += f.bands[b];   /* 30..~180 Hz: a 100 Hz tone lands near band 12 */
	for (int b = 40; b < FF_BANDS; b++) high += f.bands[b];
	CHECK(low > 0.5f);
	CHECK(high < 0.2f * low);
	CHECK(f.bass > f.treble);
	CHECK_NEAR(f.level, 0.5f / sqrtf(2.f), 0.08f);   /* RMS of a 0.5 amplitude sine */
	CHECK(f.peak > 0.4f && f.peak <= 1.0f);

	/* waveform carries the last samples: values within -1..1 and not all zero */
	int nonzero = 0; for (int i = 0; i < FF_WAVE; i++) { CHECK(f.wave[i] >= -1.f && f.wave[i] <= 1.f); if (f.wave[i] != 0.f) nonzero++; }
	CHECK(nonzero > FF_WAVE / 2);

	/* release: after silence the bands decay but not instantly */
	memset(buf, 0, sizeof buf);
	ff_analysis_push(a, buf, FF_HOP, &f);
	CHECK(low > 0.f && f.bands[2] > 0.f && f.bands[2] < 1.f);
	for (int i = 0; i < 100; i++) ff_analysis_push(a, buf, FF_HOP, &f);
	CHECK(f.bands[2] < 0.01f);

	/* click train at 2 Hz for 5 s → 10 ± 1 beats, beat envelope decays */
	ff_analysis_destroy(a); a = ff_analysis_create(48000); ff_analysis_set_params(a, &p);
	uint32_t before = 0; size_t sample = 0; const size_t period = 24000;
	for (int hop = 0; hop < (5 * 48000) / FF_HOP; hop++) {
		for (size_t i = 0; i < FF_HOP; i++, sample++) buf[i] = (sample % period) < 96 ? 0.9f : 0.f;
		ff_analysis_push(a, buf, FF_HOP, &f);
	}
	CHECK(f.beat_count >= 9 && f.beat_count <= 11);
	before = f.beat_count;
	float b0 = f.beat;
	memset(buf, 0, sizeof buf);
	ff_analysis_push(a, buf, FF_HOP, &f); ff_analysis_push(a, buf, FF_HOP, &f);
	CHECK(f.beat <= b0);
	CHECK(f.beat_count == before);

	/* gain: +12 dB on a quiet tone raises level */
	ff_analysis_destroy(a); a = ff_analysis_create(48000); ff_analysis_set_params(a, &p);
	ph = 0; for (int i = 0; i < 40; i++) { tone(buf, FF_HOP, 440.f, sr, 0.05f, &ph); ff_analysis_push(a, buf, FF_HOP, &f); }
	float quiet = f.level;
	p.gain_db = 12.f; ff_analysis_set_params(a, &p);
	for (int i = 0; i < 40; i++) { tone(buf, FF_HOP, 440.f, sr, 0.05f, &ph); ff_analysis_push(a, buf, FF_HOP, &f); }
	CHECK(f.level > quiet * 3.0f);

	ff_analysis_destroy(a);
	FF_TEST_MAIN_END();
}
