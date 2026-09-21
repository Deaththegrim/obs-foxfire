/* Mouth shapes from audio.
 *
 * The vowels below are SYNTHESISED from published formant frequencies, through a source-filter
 * model -- a glottal pulse train into three resonators -- and then pushed through this engine's
 * real analysis. That matters: it means the test signal's correct answer is known by
 * construction, not by listening to it. If "oo" stops coming out as a pucker, either the
 * classifier or the band layout changed, and both are worth failing over.
 *
 * ARMED by mutation -- each decision inverted in turn, recompiled and rerun:
 *
 *     control (every guard in place)                 103 checks,  0 failed
 *     wide-open threshold back to the old 0.65       103 checks,  1 failed
 *     front/back threshold moved off the median      103 checks,  1 failed
 *     jaw threshold removed (never closed)           103 checks,  9 failed
 *     hold time ignored                              103 checks,  2 failed
 *     closure treated as rest                        103 checks,  1 failed
 *     closure never becomes rest                     103 checks,  1 failed
 *     no immediate attack out of rest                103 checks,  3 failed
 *     silence does not close the mouth               103 checks,  1 failed
 *     band layout off by an octave                   103 checks, 12 failed
 *
 * The first two are caught ONLY by the percentile checks. Every synthetic vowel classifies
 * perfectly with the old 0.65 threshold -- which is precisely how it shipped in the first place.
 *
 * Recorded speech was used too, and is not in here: it lives outside this repo, and a test that
 * silently skips when a file is missing is a test that passes having inspected nothing. What the
 * recordings were for is written into ff-viseme.c as the measured distribution of both features
 * over 638 voiced frames -- which is what set the thresholds, and which caught a threshold that
 * was right about the physics and wrong about every real voice (wide-open never fired at all).
 */

#include "ff-test.h"
#include <ff-analysis.h>
#include <ff-viseme.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SR 48000

/* One two-pole resonator -- a formant. */
struct reso {
	float y1, y2, a1, a2;
};

static void reso_set(struct reso *r, float hz, float bw)
{
	float rr = expf(-3.14159265f * bw / SR);
	float th = 2.0f * 3.14159265f * hz / SR;
	r->a1 = 2.0f * rr * cosf(th);
	r->a2 = -rr * rr;
	r->y1 = r->y2 = 0.0f;
}

static float reso_run(struct reso *r, float x)
{
	float y = x + r->a1 * r->y1 + r->a2 * r->y2;
	r->y2 = r->y1;
	r->y1 = y;
	return y;
}

/* A vowel: glottal pulses at f0 shaped by three formants, then normalised.
 *
 * The normalisation is not cosmetic. Cascaded high-Q resonators have enormous and
 * formant-dependent gain -- measured peaks of 58, 78 and 1414 from the same input -- so without
 * it the analysis sees a clipped signal and every vowel reads the same. That was the first
 * version of this, and it classified all five vowels identically. */
static void synth_vowel(float *out, size_t n, float f0, float f1, float f2, float f3)
{
	struct reso r1, r2, r3;
	reso_set(&r1, f1, 80.0f);
	reso_set(&r2, f2, 100.0f);
	reso_set(&r3, f3, 150.0f);
	float phase = 0.0f, step = f0 / (float)SR;
	for (size_t i = 0; i < n; i++) {
		phase += step;
		float pulse = 0.0f;
		if (phase >= 1.0f) {
			phase -= 1.0f;
			pulse = 1.0f;
		}
		out[i] = reso_run(&r3, reso_run(&r2, reso_run(&r1, pulse)));
	}
	float mx = 0.0f;
	for (size_t i = 0; i < n; i++)
		if (fabsf(out[i]) > mx)
			mx = fabsf(out[i]);
	if (mx > 1e-9f)
		for (size_t i = 0; i < n; i++)
			out[i] *= 0.3f / mx;
}

/* Runs a buffer through the analysis and returns the last frame it produced. */
static bool last_frame(const float *pcm, size_t n, struct ff_frame *out)
{
	struct ff_analysis *a = ff_analysis_create(SR);
	bool got = false;
	for (size_t off = 0; off + 512 <= n; off += 512) {
		struct ff_frame t;
		if (ff_analysis_push(a, pcm + off, 512, &t)) {
			*out = t;
			got = true;
		}
	}
	ff_analysis_destroy(a);
	return got;
}

int main(void)
{
	static float buf[SR];
	struct ff_frame f;
	struct ff_viseme_params p;
	ff_viseme_defaults(&p);

	/* ---- the bands are where the classifier thinks they are ---- */
	CHECK(ff_viseme_band_hz(0) > 30.0f && ff_viseme_band_hz(0) < 34.0f);
	CHECK(ff_viseme_band_hz(FF_BANDS - 1) > 14000.0f);
	for (int i = 1; i < FF_BANDS; i++)
		CHECK(ff_viseme_band_hz(i) > ff_viseme_band_hz(i - 1));
	/* out of range is clamped, not read off the end of anything */
	CHECK(ff_viseme_band_hz(-5) == ff_viseme_band_hz(0));
	CHECK(ff_viseme_band_hz(9999) == ff_viseme_band_hz(FF_BANDS - 1));

	/* ---- the boundaries sit somewhere a real voice actually goes ----
	 *
	 * Synthetic vowels cannot check this and it is not a detail: the wide-open threshold was
	 * once 0.65, all five vowels below classified perfectly, and the shape never appeared on
	 * twelve seconds of real speech because 99% of voiced frames never reach 0.63. These pin
	 * each boundary inside the range that recorded speech actually occupies. */
	CHECK(FF_VIS_OPEN_WIDE < FF_VIS_P90_OPEN);
	CHECK(FF_VIS_OPEN_WIDE > FF_VIS_P50_OPEN);
	CHECK(FF_VIS_JAW > FF_VIS_P50_OPEN - 0.05f && FF_VIS_JAW < FF_VIS_P90_OPEN);
	CHECK(FF_VIS_FRONT > FF_VIS_P25_FRONT);
	CHECK(FF_VIS_FRONT < FF_VIS_P75_FRONT);
	/* and they are ordered: a wide mouth is more open than an open one */
	CHECK(FF_VIS_OPEN_WIDE > FF_VIS_JAW);

	/* ---- each vowel lands on the shape an artist drew for it ----
	   F1/F2/F3 from the published tables; the expected shape from Rhubarb's own descriptions
	   of what A-F are for. */
	struct {
		const char *name;
		float f1, f2, f3;
		enum ff_viseme want;
	} V[] = {
		{"ee (fleece)", 240, 2400, 2900, FF_VIS_B},  /* closed, teeth together */
		{"eh (dress)", 390, 2300, 2800, FF_VIS_C},   /* open */
		{"ae (trap)", 850, 1610, 2600, FF_VIS_D},    /* wide open */
		{"aw (thought)", 360, 640, 2400, FF_VIS_E},  /* slightly rounded */
		{"oo (goose)", 250, 595, 2400, FF_VIS_F},    /* puckered */
	};
	for (size_t k = 0; k < sizeof V / sizeof V[0]; k++) {
		synth_vowel(buf, SR, 120.0f, V[k].f1, V[k].f2, V[k].f3);
		CHECK(last_frame(buf, SR, &f));
		enum ff_viseme got = ff_viseme_classify(&f);
		CHECK(got == V[k].want);
		if (got != V[k].want)
			fprintf(stderr, "      (%s at F1=%.0f F2=%.0f gave %s, wanted %s)\n",
				V[k].name, V[k].f1, V[k].f2, ff_viseme_name(got),
				ff_viseme_name(V[k].want));
	}

	/* A DIFFERENT speaker: a higher pitch with the same formants is the same vowel. Formants
	   come from the shape of the mouth and the fundamental does not, so a voice an octave up
	   must not change the shape -- otherwise the rig works for one person only. */
	synth_vowel(buf, SR, 220.0f, 250, 595, 2400);
	CHECK(last_frame(buf, SR, &f));
	CHECK(ff_viseme_classify(&f) == FF_VIS_F);
	synth_vowel(buf, SR, 220.0f, 240, 2400, 2900);
	CHECK(last_frame(buf, SR, &f));
	CHECK(ff_viseme_classify(&f) == FF_VIS_B);

	/* silence is rest, not a guess */
	memset(buf, 0, sizeof buf);
	CHECK(last_frame(buf, SR, &f));
	CHECK(ff_viseme_classify(&f) == FF_VIS_X);

	/* ---- timing: the half that decides whether this reads as speech ---- */
	struct ff_viseme_state s;
	ff_viseme_init(&s);
	CHECK(s.current == FF_VIS_X);

	struct ff_frame loud, quiet;
	memset(&quiet, 0, sizeof quiet);
	synth_vowel(buf, SR, 120.0f, 240, 2400, 2900); /* "ee" -> B */
	CHECK(last_frame(buf, SR, &loud));

	/* the first frame of a word is already the right shape: no ramp-in */
	CHECK(ff_viseme_update(&s, &loud, 16.0f, &p) == FF_VIS_B);

	/* a different vowel does NOT take hold until the hold time has passed */
	struct ff_frame other;
	synth_vowel(buf, SR, 120.0f, 250, 595, 2400); /* "oo" -> F */
	CHECK(last_frame(buf, SR, &other));
	CHECK(ff_viseme_update(&s, &other, 16.0f, &p) == FF_VIS_B);
	CHECK(ff_viseme_update(&s, &other, 16.0f, &p) == FF_VIS_B);
	for (int i = 0; i < 10; i++)
		ff_viseme_update(&s, &other, 16.0f, &p);
	CHECK(s.current == FF_VIS_F);

	/* A SHORT gap is a closure -- "p", "b", "m" are made by shutting the mouth, and the shut
	   mouth is the shape. Ignores the hold, because a closure is fast. */
	CHECK(ff_viseme_update(&s, &quiet, 16.0f, &p) == FF_VIS_A);

	/* a LONG gap is the speaker stopping, which is rest */
	for (float t = 0; t < p.closure_ms + 50.0f; t += 16.0f)
		ff_viseme_update(&s, &quiet, 16.0f, &p);
	CHECK(s.current == FF_VIS_X);

	/* and speech starts again immediately, without waiting out a hold */
	CHECK(ff_viseme_update(&s, &loud, 16.0f, &p) == FF_VIS_B);

	/* A mouth that has never heard anything rests. Without this a source added to a silent
	   scene sits there with its mouth clamped shut, which reads as sulking. */
	struct ff_viseme_state fresh;
	ff_viseme_init(&fresh);
	CHECK(ff_viseme_update(&fresh, &quiet, 16.0f, &p) == FF_VIS_X);

	CHECK(strcmp(ff_viseme_name(FF_VIS_A), "A") == 0);
	CHECK(strcmp(ff_viseme_name(FF_VIS_X), "X") == 0);
	CHECK(strcmp(ff_viseme_name((enum ff_viseme)999), "?") == 0);

	FF_TEST_MAIN_END();
}
