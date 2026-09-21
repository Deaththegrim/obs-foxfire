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
 *     control (every guard in place)                 201 checks,  0 failed
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
 * for the timings, which a reviewer panel found were exercised only at their defaults -- both
 * outcomes of the closure split fired, but replacing p->closure_ms with a literal 200 left
 * everything green, and `openness` was read by no check at all so the whole release smoother
 * could be replaced with `s->openness = f->level;` (at 276 checks):
 *
 *     release_ms ignored, openness follows level     276 checks,  2 failed
 *     the dt/release clamp removed                   276 checks,  1 failed
 *     release_ms <= 0 falls through to the smoother  276 checks,  2 failed
 *     closure_ms ignored (a literal 200)             276 checks,  1 failed
 *     no immediate attack out of a CLOSURE           276 checks,  1 failed
 *     jaw bias lower clamp removed                   276 checks,  1 failed
 *     jaw bias upper clamp removed                   276 checks,  0 failed  <- see below
 *
 * The last row is honest rather than fixed. At the clamped +0.15 the wide-open line sits at
 * 0.41 and every vowel this file can synthesise is above it -- the lowest, "ee", measures
 * 0.4159 -- so a check on the positive side reads D at both the clamped and the unclamped value
 * and agrees for the wrong reason. Downward there is room and that half is armed.
 *
 * "no immediate attack out of a CLOSURE" is the one worth reading twice: a voiced frame never
 * once followed an A in any fixture, because every closure here was followed by more silence.
 * Deleting `|| s->current == FF_VIS_A` left all 250 checks green, and that branch is what makes
 * a /p/ open on time instead of waiting out an 80 ms hold.
 *
 * for the jaw bias (at 250 checks):
 *
 *     bias ignored in the classifier                 250 checks,  4 failed
 *     bias sign flipped                              250 checks,  8 failed
 *     bias applied to the wide line only             250 checks,  2 failed
 *     update() passing 0 instead of the setting      250 checks,  2 failed
 *     default bias nudged off zero                   250 checks,  2 failed
 *
 * The fourth of those SURVIVED at first, clean: every bias check called the classifier directly,
 * so none of them said anything about whether the SETTING reaches it. The wiring from the params
 * struct is now driven through ff_viseme_update, which is the only path the plugin takes.
 *
 * and for the frication half, which came earlier (numbers re-measured at 201 checks):
 *
 *     frication branch removed                       201 checks,  6 failed
 *     threshold lowered to 0.05                      201 checks, 13 failed
 *     threshold raised to 0.50                       201 checks,  7 failed
 *     high band starting at 2 kHz                    201 checks, 28 failed
 *     frication reading the voiced half              201 checks, 36 failed
 *     the two halves of the split swapped            201 checks, 48 failed
 *
 * The 2 kHz mutant SURVIVED at first, at a clean 0 failed, and finding out why was the point:
 * ff_viseme_frication and ff_viseme_classify each computed the split themselves, so moving the
 * line in one left the other -- the one every test reads -- saying the old thing. One
 * split_bands() now, and the mutant takes 28 checks with it.
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
#include <stdint.h>
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

/* Deterministic noise. rand() would make a failure depend on the C library, and a fixture whose
   value changes between machines is a fixture nobody can argue with. */
static uint32_t nseed = 1u;
static float noise(void)
{
	nseed = nseed * 1664525u + 1013904223u; /* Numerical Recipes LCG */
	return (float)(nseed >> 8) / 8388608.0f - 1.0f;
}

static void normalise(float *out, size_t n)
{
	float mx = 0.0f;
	for (size_t i = 0; i < n; i++)
		if (fabsf(out[i]) > mx)
			mx = fabsf(out[i]);
	if (mx > 1e-9f)
		for (size_t i = 0; i < n; i++)
			out[i] *= 0.3f / mx;
}

/* A vowel with the high end MODELLED rather than missing: five formants and a lip-radiation
 * term, plus optional aspiration noise at the glottis.
 *
 * synth_vowel above is deliberately kept as it is -- it is the fixture the vowel thresholds were
 * set against -- but it cannot say anything about frication, because three resonators roll off
 * 36 dB/octave past F3 and leave exactly nothing above 3.5 kHz. Measured: every vowel it makes
 * reads frication 0.0000 at every aspiration level up to -6 dB, which would justify any
 * threshold at all. Real tracts have formants near 3.5 and 4.5 kHz, and radiation from the lips
 * is a differentiator: +6 dB/octave, lifting the top end rather than burying it.
 *
 * `asp` is the aspiration amplitude relative to the glottal pulse; 0 is a clean voice. */
static void synth_vowel5(float *out, size_t n, float f0, float f1, float f2, float f3, float asp)
{
	struct reso r1, r2, r3, r4, r5;
	reso_set(&r1, f1, 80.0f);
	reso_set(&r2, f2, 100.0f);
	reso_set(&r3, f3, 150.0f);
	reso_set(&r4, 3500.0f, 200.0f);
	reso_set(&r5, 4500.0f, 250.0f);
	float phase = 0.0f, step = f0 / (float)SR, prev = 0.0f;
	for (size_t i = 0; i < n; i++) {
		phase += step;
		float x = 0.0f;
		if (phase >= 1.0f) {
			phase -= 1.0f;
			x = 1.0f;
		}
		/* at the SOURCE: aspiration is turbulence at the glottis and goes through the same
		   tract. Added to the output instead it would be room noise, which is a different
		   signal and a much easier one to tell from a vowel. */
		x += noise() * asp;
		float y = reso_run(&r5, reso_run(&r4, reso_run(&r3, reso_run(&r2, reso_run(&r1, x)))));
		out[i] = y - prev; /* radiation */
		prev = y;
	}
	normalise(out, n);
}

/* A fricative: noise through one broad resonator, and the SAME radiation term as the vowel.
   Applying it to one and not the other would be the separation being measured. `buzz` > 0 adds a
   voicing bar underneath, which is what makes /v/ a /v/ and not an /f/. */
static void synth_fric(float *out, size_t n, float hz, float bw, float buzz)
{
	struct reso r, vb;
	reso_set(&r, hz, bw);
	reso_set(&vb, 250.0f, 60.0f);
	float phase = 0.0f, step = buzz / (float)SR, prev = 0.0f;
	for (size_t i = 0; i < n; i++) {
		float v = reso_run(&r, noise());
		if (buzz > 0.0f) {
			phase += step;
			float pulse = 0.0f;
			if (phase >= 1.0f) {
				phase -= 1.0f;
				pulse = 1.0f;
			}
			v += reso_run(&vb, pulse) * 0.5f;
		}
		out[i] = v - prev;
		prev = v;
	}
	normalise(out, n);
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
		enum ff_viseme got = ff_viseme_classify(&f, 0.0f);
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
	CHECK(ff_viseme_classify(&f, 0.0f) == FF_VIS_F);
	synth_vowel(buf, SR, 220.0f, 240, 2400, 2900);
	CHECK(last_frame(buf, SR, &f));
	CHECK(ff_viseme_classify(&f, 0.0f) == FF_VIS_B);

	/* ---- the jaw bias: one voice's openness is not another's ---- */

	/* One frame, three shapes, decided only by the trim. "eh" sits at openness 0.527, between
	   the two thresholds, which is what makes it the frame that can show all three. */
	synth_vowel(buf, SR, 110.0f, 390, 2300, 3000);
	CHECK(last_frame(buf, SR, &f));
	CHECK(ff_viseme_classify(&f, 0.0f) == FF_VIS_C);
	CHECK(ff_viseme_classify(&f, 0.05f) == FF_VIS_D);  /* + opens: crosses the wide line */
	CHECK(ff_viseme_classify(&f, -0.10f) == FF_VIS_B); /* - closes: falls under the jaw line */

	/* The bias is CLAMPED where it is used, not merely bounded by the slider. Settings arrive
	   over obs-websocket and out of hand-edited scene JSON, and mouth-proof.py already pushes
	   hold_ms to 8000 against a maximum of 250 by that route. Unclamped, a bias past about
	   +-0.5 puts a threshold outside the range openness occupies and the mouth sticks on one
	   shape with no diagnostic. */
	{
		/* Armed on the NEGATIVE side only, and the reason is worth writing down: at the
		   clamped +0.15 the wide-open line sits at 0.41, and every vowel this file can
		   synthesise is above it (the lowest, "ee", measures 0.4159). So a positive check
		   reads D at both the clamped and the unclamped value and agrees for the wrong
		   reason -- which is exactly what the first version of this did, written against
		   "eh", and it passed with the clamp deleted.
		   Downward there is room: "ae" at 0.776 is still D at -0.15 (the line moves to
		   0.71) and is NOT D unclamped at -5.0. */
		struct ff_frame cf;
		synth_vowel(buf, SR, 110.0f, 850, 1610, 2600);
		CHECK(last_frame(buf, SR, &cf));
		CHECK(ff_viseme_classify(&cf, -FF_VIS_JAW_BIAS_MAX) == FF_VIS_D);
		CHECK(ff_viseme_classify(&cf, -5.0f) == FF_VIS_D);
		CHECK(ff_viseme_classify(&cf, 5.0f) == ff_viseme_classify(&cf, FF_VIS_JAW_BIAS_MAX));
	}

	/* The sign, stated as a property rather than as three examples: raising the bias can only
	   ever move a frame toward a MORE open shape, never back. Checked across the whole vowel
	   set so it cannot pass on one lucky frame. */
	for (size_t k = 0; k < sizeof V / sizeof V[0]; k++) {
		synth_vowel(buf, SR, 110.0f, V[k].f1, V[k].f2, V[k].f3);
		CHECK(last_frame(buf, SR, &f));
		int prev = -1;
		for (float bias = -0.15f; bias <= 0.1501f; bias += 0.05f) {
			/* rank by jaw aperture, not by the enum, which is drawn in art order:
			   B and F are the closed pair, C and E the middle, D the wide one */
			enum ff_viseme g = ff_viseme_classify(&f, bias);
			int rank = (g == FF_VIS_D) ? 2 : (g == FF_VIS_C || g == FF_VIS_E) ? 1 : 0;
			CHECK(rank >= prev);
			prev = rank;
		}
	}

	/* And zero is exactly the old behaviour -- the vowel sweep above ran at 0 and every shape
	   came out as it did before the trim existed. That is what makes 0 a safe default rather
	   than a number that happens to look neutral. */
	ff_viseme_defaults(&p);
	CHECK(p.jaw_bias == 0.0f);

	/* Through ff_viseme_update, which is the only path the plugin ever takes. Everything above
	   calls the classifier directly and so says nothing about whether the SETTING reaches it --
	   a mutation replacing p->jaw_bias with 0 at the call site survived all of it cleanly. */
	{
		struct ff_viseme_state js;
		struct ff_viseme_params jp;
		synth_vowel(buf, SR, 110.0f, 390, 2300, 3000);
		CHECK(last_frame(buf, SR, &f));
		ff_viseme_defaults(&jp);
		ff_viseme_init(&js);
		CHECK(ff_viseme_update(&js, &f, 16.0f, &jp) == FF_VIS_C);
		jp.jaw_bias = 0.05f;
		ff_viseme_init(&js);
		CHECK(ff_viseme_update(&js, &f, 16.0f, &jp) == FF_VIS_D);
		jp.jaw_bias = -0.10f;
		ff_viseme_init(&js);
		CHECK(ff_viseme_update(&js, &f, 16.0f, &jp) == FF_VIS_B);
	}

	/* ---- frication: a hiss is not a vowel ---- */

	/* Both sides of the threshold, as FEATURE values rather than shapes. The shape alone
	   cannot referee this: "ee" is B, and a hiss is now B, so a check that only looked at the
	   shape would pass with the feature reading anything at all. */
	static const struct {
		const char *name;
		float hz, bw, buzz;
	} FR[] = {
		{"s, peak 6.5k", 6500.0f, 4000.0f, 0.0f},
		{"s, peak 5.2k", 5200.0f, 3000.0f, 0.0f},
		{"sh, peak 3.2k", 3200.0f, 2500.0f, 0.0f},
		{"sh, dark 2.6k", 2600.0f, 2000.0f, 0.0f},
		{"f, flat and broad", 4500.0f, 7000.0f, 0.0f},
		{"v, voiced", 4500.0f, 7000.0f, 110.0f},
	};
	float worst_fric = 1.0f;
	for (size_t k = 0; k < sizeof FR / sizeof FR[0]; k++) {
		synth_fric(buf, SR, FR[k].hz, FR[k].bw, FR[k].buzz);
		CHECK(last_frame(buf, SR, &f));
		float fr = ff_viseme_frication(&f);
		if (fr < worst_fric)
			worst_fric = fr;
		CHECK(fr > FF_VIS_FRICATION);
		/* the teeth-together shape, never the open jaw it drew before this existed */
		enum ff_viseme got = ff_viseme_classify(&f, 0.0f);
		CHECK(got == FF_VIS_B);
		if (got != FF_VIS_B)
			fprintf(stderr, "      (%s: frication %.4f gave %s, wanted B)\n", FR[k].name, fr,
				ff_viseme_name(got));
	}

	/* And the other side: a vowel stays under the threshold however breathy it is. The
	   aspiration sweep is the point -- a clean vowel is easy, and the case that would break
	   this is a breathy or bright voice, not a studio one. */
	static const struct {
		float f1, f2, f3;
	} VF[] = {{240, 2400, 2900}, {390, 2300, 3000}, {850, 1610, 2600}, {360, 640, 2400}, {250, 595, 2400}};
	static const float ASP[] = {0.0f, 0.05f, 0.1f, 0.2f, 0.32f, 0.5f}; /* clean to about -6 dB */
	float worst_vowel = 0.0f;
	for (size_t k = 0; k < sizeof VF / sizeof VF[0]; k++)
		for (size_t a = 0; a < sizeof ASP / sizeof ASP[0]; a++) {
			synth_vowel5(buf, SR, 110.0f, VF[k].f1, VF[k].f2, VF[k].f3, ASP[a]);
			CHECK(last_frame(buf, SR, &f));
			float fr = ff_viseme_frication(&f);
			if (fr > worst_vowel)
				worst_vowel = fr;
			CHECK(fr < FF_VIS_FRICATION);
			/* "ae" is the one vowel that keeps its SHAPE right across this sweep, so it
			   is the one asserted on. A feature check alone cannot catch frication
			   stealing a vowel, because "ee" is legitimately B either way. The others
			   move under the five-formant model and that is a finding, not a test: see
			   the note on the vowel thresholds in ff-viseme.h. */
			if (VF[k].f1 == 850)
				CHECK(ff_viseme_classify(&f, 0.0f) == FF_VIS_D);
		}

	/* The margin itself, so a threshold creeping toward either family fails here rather than
	   in somebody's stream. Both numbers are printed because a gate that narrows quietly is
	   the one nobody notices. */
	fprintf(stderr, "      frication: vowels reach %.4f, fricatives fall to %.4f, threshold %.2f\n",
		worst_vowel, worst_fric, FF_VIS_FRICATION);
	CHECK(worst_vowel < FF_VIS_FRICATION * 0.6f);
	CHECK(worst_fric > FF_VIS_FRICATION * 1.6f);

	/* LEVEL. Frication looks like a share of the spectrum and is not one -- the bands are
	   dB-mapped with a -60 dB floor, so it moves with gain. This was written first as an
	   invariance check, which failed immediately and was right to: see band_sum() in
	   ff-viseme.c. What actually has to hold is weaker and is what is checked here -- the drift
	   pushes both families AWAY from the threshold, so turning a signal down never turns a
	   vowel into a hiss.

	   Quarter level, not a tenth: a dark /sh/ does fall through at a tenth (0.275 measured,
	   under the threshold), and that limit is written down rather than tested around. At a
	   tenth of this level the frame is under the default noise gate and the mouth is shut. */
	float quiet_v = 0.0f, quiet_f = 1.0f;
	for (float scale = 1.0f; scale > 0.2f; scale *= 0.5f) {
		synth_fric(buf, SR, 5200.0f, 3000.0f, 0.0f);
		for (size_t i = 0; i < SR; i++)
			buf[i] *= scale;
		CHECK(last_frame(buf, SR, &f));
		float fr = ff_viseme_frication(&f);
		if (fr < quiet_f)
			quiet_f = fr;
		CHECK(fr > FF_VIS_FRICATION);

		synth_vowel5(buf, SR, 110.0f, 240, 2400, 2900, 0.2f);
		for (size_t i = 0; i < SR; i++)
			buf[i] *= scale;
		CHECK(last_frame(buf, SR, &f));
		fr = ff_viseme_frication(&f);
		if (fr > quiet_v)
			quiet_v = fr;
		CHECK(fr < FF_VIS_FRICATION);
	}
	fprintf(stderr, "      frication down to quarter level: vowel <= %.4f, /s/ >= %.4f\n", quiet_v,
		quiet_f);

	/* ---- the timings, driven off their defaults ---- */

	/* release_ms was observed by NOTHING before this. `openness` is never read by any other
	   check here, and mouth-proof's two readings of it are taken after a three-second settle
	   against a slider that stops at 500 ms, so the decay has finished either way. Replacing
	   the whole smoother with `s->openness = f->level;` passed every check in both files.
	   Pure arithmetic, so it is checked as arithmetic: k = dt/release, applied once. */
	{
		struct ff_viseme_state rs;
		struct ff_viseme_params rp;
		ff_viseme_defaults(&rp);
		ff_viseme_init(&rs);
		synth_vowel(buf, SR, 110.0f, 850, 1610, 2600);
		CHECK(last_frame(buf, SR, &f)); /* loud */
		struct ff_frame hush;
		memset(buf, 0, sizeof buf);
		CHECK(last_frame(buf, SR, &hush));

		rs.openness = 1.0f;
		rp.release_ms = 100.0f;
		ff_viseme_update(&rs, &hush, 50.0f, &rp); /* k = 0.5 */
		CHECK(fabsf(rs.openness - 0.5f) < 1e-4f);

		rs.openness = 1.0f;
		rp.release_ms = 400.0f;
		ff_viseme_update(&rs, &hush, 50.0f, &rp); /* k = 0.125 */
		CHECK(fabsf(rs.openness - 0.875f) < 1e-4f);

		/* dt past the release time: the k clamp, which no fixture reached because every
		   other update here uses 16 ms against a default of 120 */
		rs.openness = 1.0f;
		rp.release_ms = 20.0f;
		ff_viseme_update(&rs, &hush, 50.0f, &rp);
		CHECK(rs.openness == 0.0f);

		/* and zero, which the slider's own minimum offers */
		rs.openness = 1.0f;
		rp.release_ms = 0.0f;
		ff_viseme_update(&rs, &hush, 16.0f, &rp);
		CHECK(rs.openness == 0.0f);

		/* release 0 AND a zero-length frame. Without the explicit branch this is 0/0, and
		   the smoother becomes openness += (target - openness) * NaN -- a mouth that is
		   NaN open for the rest of the session. The clamp does not save it: NaN > 1.0 is
		   false, so nothing clamps. Removing the branch passes every other check here,
		   because every other one has a dt. */
		rs.openness = 1.0f;
		rp.release_ms = 0.0f;
		ff_viseme_update(&rs, &hush, 0.0f, &rp);
		CHECK(rs.openness == rs.openness); /* i.e. not NaN */
		CHECK(rs.openness == 0.0f);

		/* rising is immediate whatever the release says -- a mouth that lags the attack of
		   a word looks dubbed */
		rp.release_ms = 500.0f;
		rs.openness = 0.0f;
		ff_viseme_update(&rs, &f, 16.0f, &rp);
		CHECK(rs.openness == f.level);
	}

	/* closure_ms: both OUTCOMES were exercised, the SETTING was not. Replacing p->closure_ms
	   with the literal 200.0f left every check green -- which is verbatim the mutation the jaw
	   bias was armed against three sections up. The lesson had been applied to one control. */
	{
		struct ff_viseme_state cs;
		struct ff_viseme_params cp;
		struct ff_frame hush;
		ff_viseme_defaults(&cp);
		synth_vowel(buf, SR, 110.0f, 850, 1610, 2600);
		CHECK(last_frame(buf, SR, &f));
		memset(buf, 0, sizeof buf);
		CHECK(last_frame(buf, SR, &hush));

		cp.closure_ms = 50.0f;
		ff_viseme_init(&cs);
		ff_viseme_update(&cs, &f, 16.0f, &cp); /* speak, so the mouth has something to shut */
		ff_viseme_update(&cs, &hush, 16.0f, &cp);
		CHECK(cs.current == FF_VIS_A); /* 16 ms is a closure at 50 */
		ff_viseme_update(&cs, &hush, 16.0f, &cp);
		ff_viseme_update(&cs, &hush, 16.0f, &cp);
		ff_viseme_update(&cs, &hush, 16.0f, &cp); /* 64 ms: past 50, so the speaker stopped */
		CHECK(cs.current == FF_VIS_X);

		/* the same four frames at the shipped 200 ms are still a closure */
		ff_viseme_defaults(&cp);
		ff_viseme_init(&cs);
		ff_viseme_update(&cs, &f, 16.0f, &cp);
		for (int i = 0; i < 4; i++)
			ff_viseme_update(&cs, &hush, 16.0f, &cp);
		CHECK(cs.current == FF_VIS_A);
	}

	/* The STOP RELEASE: the first voiced frame after a closure is already the new shape, with
	   no hold to wait out. A voiced frame never once followed an A in any fixture -- every
	   closure here was followed by more silence -- so deleting `|| s->current == FF_VIS_A`
	   from the immediate-attack test left all 250 checks green. That branch is what makes a
	   /p/ open on time; without it the mouth waits 80 ms after every stop, which is the
	   dubbed look the immediate path exists to prevent. */
	{
		struct ff_viseme_state ps;
		struct ff_viseme_params pp;
		struct ff_frame hush;
		ff_viseme_defaults(&pp);
		ff_viseme_init(&ps);
		synth_vowel(buf, SR, 110.0f, 240, 2400, 2900); /* "ee" -> B */
		CHECK(last_frame(buf, SR, &f));
		memset(buf, 0, sizeof buf);
		CHECK(last_frame(buf, SR, &hush));
		synth_vowel(buf, SR, 110.0f, 850, 1610, 2600); /* "ae" -> D */
		struct ff_frame wide;
		CHECK(last_frame(buf, SR, &wide));

		CHECK(ff_viseme_update(&ps, &f, 16.0f, &pp) == FF_VIS_B);
		CHECK(ff_viseme_update(&ps, &hush, 16.0f, &pp) == FF_VIS_A); /* the closure */
		/* one frame later, and a DIFFERENT shape: no hold, no waiting */
		CHECK(ff_viseme_update(&ps, &wide, 16.0f, &pp) == FF_VIS_D);
	}

	/* the frication accessor's own silence guard, which the classifier's guard hides */
	memset(buf, 0, sizeof buf);
	CHECK(last_frame(buf, SR, &f));
	CHECK(ff_viseme_frication(&f) == 0.0f);

	/* silence is rest, not a guess */
	memset(buf, 0, sizeof buf);
	CHECK(last_frame(buf, SR, &f));
	CHECK(ff_viseme_classify(&f, 0.0f) == FF_VIS_X);

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
