#include "ff-viseme.h"

#include <math.h>
#include <string.h>

/* The engine lays its 64 bands out logarithmically from 30 Hz to 16 kHz. */
#define FF_LO_HZ 30.0f
#define FF_HI_HZ 16000.0f

float ff_viseme_band_hz(int i)
{
	if (i < 0)
		i = 0;
	if (i >= FF_BANDS)
		i = FF_BANDS - 1;
	/* the band's CENTRE, hence the +0.5 */
	return FF_LO_HZ * powf(FF_HI_HZ / FF_LO_HZ, ((float)i + 0.5f) / (float)FF_BANDS);
}

void ff_viseme_defaults(struct ff_viseme_params *p)
{
	p->gate = 0.04f;
	p->closure_ms = 200.0f;
	/* 80 ms: roughly the length of a spoken phoneme, and the number that decides whether this
	   reads as speech or as flapping. */
	p->hold_ms = 80.0f;
	p->release_ms = 120.0f;
	/* 0: the thresholds as measured. Every voice this was checked against classifies sanely
	   without a trim -- the control is for the one that does not. */
	p->jaw_bias = 0.0f;
}

void ff_viseme_init(struct ff_viseme_state *s)
{
	memset(s, 0, sizeof *s);
	s->current = FF_VIS_X;
}

const char *ff_viseme_name(enum ff_viseme v)
{
	static const char *N[FF_VISEME_COUNT] = {"A", "B", "C", "D", "E", "F", "G", "H", "X"};
	if (v < 0 || v >= FF_VISEME_COUNT)
		return "?";
	return N[v];
}

/* Sums the bands whose centres fall between two frequencies.
 *
 * NOT energy, whatever the shape of this function suggests. ff-analysis maps each band through
 * db_to_unit(): 0..1 across a 60 dB window, everything quieter than -60 dB clamped to 0. So this
 * is a sum of loudness-ish numbers, and every ratio built from it -- openness, frontness,
 * frication -- drifts with the overall level instead of being the share of the spectrum it
 * looks like. Measured, frication of one signal at five gains:
 *
 *     /s/ at 5.2 kHz    0.636  0.729  0.795  0.982  1.000   <- rises: its quiet LOW bands floor
 *     "ee"              0.061  0.042  0.014  0.000  0.000   <- falls: its quiet HIGH bands floor
 *                       x1.0   x0.5   x0.25  x0.1   x0.05
 *
 * Both move AWAY from the threshold between them, which is why this is survivable rather than a
 * defect: quiet makes a fricative look more like a fricative and a vowel less like one. The
 * exception is a dark /sh/, which falls to 0.275 at a tenth of level and is missed -- but a tenth
 * of this level is under the default noise gate, so the mouth is closed there anyway.
 *
 * The principled fix is to undo db_to_unit and work in linear amplitude, which would make these
 * true spectral shares. It is not done here because every threshold in this file was set in this
 * domain, and re-setting them needs a corpus this repo does not have. */
static float band_sum(const struct ff_frame *f, float lo_hz, float hi_hz)
{
	float sum = 0.0f;
	for (int i = 0; i < FF_BANDS; i++) {
		float hz = ff_viseme_band_hz(i);
		if (hz >= lo_hz && hz <= hi_hz)
			sum += f->bands[i];
	}
	return sum;
}

/* The one place the 3.5 kHz line between "vowel" and "hiss" is drawn. It was written twice --
   once here and once in ff_viseme_classify -- and a mutation that moved it in the classifier left
   every test green, because the tests measure the feature and the feature had its own copy. */
struct band_split {
	float voiced; /* 180 Hz - 3.5 kHz: formants, the shape of the mouth */
	float high;   /* 3.5 kHz - 16 kHz: frication, sibilance, room */
};

static struct band_split split_bands(const struct ff_frame *f)
{
	struct band_split s;
	s.voiced = band_sum(f, 180.0f, 3500.0f);
	s.high = band_sum(f, 3500.0f, 16000.0f);
	return s;
}

float ff_viseme_frication(const struct ff_frame *f)
{
	struct band_split s = split_bands(f);
	float total = s.voiced + s.high;
	if (total <= 0.0001f)
		return 0.0f;
	return s.high / total;
}

enum ff_viseme ff_viseme_classify(const struct ff_frame *f, float jaw_bias)
{
	/* Voiced energy only. Everything above ~3.5 kHz is sibilance and room noise, and below
	   ~180 Hz is the fundamental and whatever the desk is resting on; neither says anything
	   about the shape of the mouth. */
	struct band_split sp = split_bands(f);
	float voiced = sp.voiced, high = sp.high;
	if (voiced + high <= 0.0001f)
		return FF_VIS_X;

	/* FRICATION, before any of the vowel reasoning, because the two ratios below are
	   MEANINGLESS on noise. An "sss" puts almost nothing in F1's range, so openness becomes
	   the quotient of two near-nothings and lands wherever the noise floor happens to put it.
	   Measured before this existed: synthesised /s/, /sh/ and /f/ every one came out as C, a
	   half-open jaw. Every sibilant in every sentence opened the mouth, and since English runs
	   somewhere near a fifth fricative by time, that is a lot of a conversation spent with the
	   jaw down on a hiss.
	 *
	 * The teeth-together B for all of it, not G. G is the F/V shape and telling /f/ from /s/
	 * is an amplitude and peak-sharpness judgement that this cannot make from 64 log bands --
	 * see ff-viseme.h. B is the honest answer: right for the sibilants and the stop releases,
	 * and closer to an /f/ than the open jaw it used to draw.
	 *
	 * ORDER MATTERS the other way too: the check above used to ask only about 180-3500 Hz, so
	 * a bright /s/ with little below 3.5 kHz fell out as X, rest. Rest is what the mouth does
	 * when the speaker has STOPPED. */
	if (high > FF_VIS_FRICATION * (voiced + high))
		return FF_VIS_B;

	if (voiced <= 0.0001f)
		return FF_VIS_X;

	/* JAW: where the energy sits inside F1's range. The low half of the range is a closed jaw
	   ("ee", "oo" at 240-250 Hz), the top is wide open ("ah" at 850). A ratio rather than a
	   peak, because a peak needs to be told which formant it found and a ratio does not. */
	float f1_low = band_sum(f, 180.0f, 450.0f);
	float f1_high = band_sum(f, 450.0f, 1100.0f);
	float openness = f1_high / (f1_low + f1_high + 1e-6f);

	/* FRONT vs BACK: F2 is up at 2300-2400 Hz for front vowels and down at ~600 Hz for back,
	   rounded ones. The gap is enormous, which is what makes this robust at 10% band spacing.
	   The low window starts above F1's range so a wide-open "ah" does not read as rounded. */
	float front = band_sum(f, 1800.0f, 3000.0f);
	float back = band_sum(f, 550.0f, 1300.0f);
	float frontness = front / (front + back + 1e-6f);

	/* The boundaries below are MEASURED, not chosen: each vowel was synthesised at its
	   published formants, pushed through this engine's own analysis, and the two features
	   read off. tests/test_viseme.c repeats that, so if the band layout or the analysis
	   changes these stop being true loudly instead of quietly.
	 *
	 *     vowel          openness   frontness
	 *     ee  240/2400      0.417      0.503
	 *     eh  390/2300      0.527      0.464
	 *     ae  850/1610      0.776      0.249
	 *     aw  360/ 640      0.525      0.000
	 *     oo  250/ 595      0.468      0.000
	 *
	 * frontness splits front from rounded by a mile (0.46+ against 0.00). openness is the
	 * tighter axis -- 0.468 for "oo" against 0.525 for "aw" is all that separates a pucker
	 * from a rounded-open mouth -- which is exactly why the hold time in ff_viseme_update
	 * matters: near a boundary the classification WILL flicker frame to frame, and the hold
	 * is what stops that reaching the screen.
	 *
	 * The thresholds satisfy TWO anchors, which is the only reason to trust them. The vowels
	 * above fix what is CORRECT: they are built from published formants, so a shape that comes
	 * out wrong there is wrong. Real speech fixes what is BALANCED.
	 *
	 * The first version put "wide open" at 0.65, which 99% of real frames never reach -- so D
	 * never appeared at all, on any speech. Synthetic vowels alone would never have shown
	 * that: "ae" hits 0.776 and classified perfectly. A threshold can be right about the
	 * physics and still be wrong about a voice.
	 *
	 * RE-MEASURED 2026-09-21 over FOUR corpora of real recorded speech, 107,259 voiced frames,
	 * four speakers -- because the distribution this was originally balanced against was 638
	 * frames of a recording that no longer exists and could not be reproduced. Run it again
	 * with tests/calibrate_cli.c.
	 *
	 *     corpus                      frames   B     C     D     E     F    open p50  front p50
	 *     48 kHz interview            31719  55.0  12.0   9.2   9.9  13.9    0.440      0.342
	 *     24 kHz clips, same speaker  14883  52.1  10.4  10.4  10.5  16.6    0.433      0.336
	 *     24 kHz clips, speaker 2     50172  38.4  13.3  23.8   8.8  15.8    0.484      0.334
	 *     24 kHz clips, speaker 3     10485  17.7   7.8  56.0   7.6  11.0    0.589      0.292
	 *
	 * What that settles and what it does not:
	 *
	 *   - NO SHAPE IS EVER DEAD. The worst case for any of B-F on any corpus is 7.6%, so the
	 *     failure that produced the 0.65 threshold cannot be hiding here.
	 *   - frontness is portable. Its median is 0.342, 0.336, 0.334, 0.292 across four different
	 *     voices, and 0.29 sits under all four. Nothing to do.
	 *   - OPENNESS IS NOT PORTABLE, and that is why jaw_bias exists. The median moves 0.156
	 *     between corpora -- nearly twice the 0.08 between the two thresholds it is compared
	 *     against -- and speaker 3 spends 56% of frames wide open. A bias of -0.10 brings that
	 *     to B 27.9 / C 11.7 / D 27.7 / E 11.9 / F 20.9. The same -0.10 applied to the first
	 *     corpus, which does not need it, degrades it to C 2.9 / D 2.3 / E 1.5. The control is
	 *     directional and 0 is right for three of the four.
	 *   - The numbers are four voices, not a population, and three of the four are 24 kHz so
	 *     their frication is measured against a 12 kHz ceiling rather than 16. Neither affects
	 *     openness or frontness, which use bands below 3 kHz.
	 *
	 * Frication, for the record, over the same corpora: 90th percentile 0.266 / 0.246 / 0.205 /
	 * 0.189 against a threshold of 0.22, so somewhere near a tenth to an eighth of voiced frames
	 * read as a hiss. That is the right order for English and is the only check that number has
	 * ever had against a real voice -- it was set from synthesis. */
	/* Both thresholds move together, so the trim slides the whole jaw axis rather than
	   squeezing the middle shape out of existence, which is what moving one of them alone
	   would do at either end of its range. */
	if (openness > FF_VIS_OPEN_WIDE - jaw_bias)
		return FF_VIS_D; /* wide open: "ah" is a wide mouth whatever the tongue is doing,
				    and deciding front-or-back at that jaw angle makes a held vowel
				    twitch between two shapes */

	bool is_front = frontness > FF_VIS_FRONT;
	if (openness > FF_VIS_JAW - jaw_bias)
		return is_front ? FF_VIS_C : FF_VIS_E;
	return is_front ? FF_VIS_B : FF_VIS_F;
}

enum ff_viseme ff_viseme_update(struct ff_viseme_state *s, const struct ff_frame *f, float dt_ms,
				const struct ff_viseme_params *p)
{
	/* Openness follows the level quickly upward and slowly downward. Rising fast matters --
	   a mouth that lags the attack of a word looks dubbed. */
	float target = f->level;
	if (target > s->openness) {
		s->openness = target;
	} else if (p->release_ms > 0.0f) {
		float k = dt_ms / p->release_ms;
		if (k > 1.0f)
			k = 1.0f;
		s->openness += (target - s->openness) * k;
	} else {
		s->openness = target;
	}

	s->held_ms += dt_ms;
	s->voiced = f->level > p->gate;

	if (!s->voiced) {
		s->silent_ms += dt_ms;
		/* A SHORT gap is a closure, not a pause: "p", "b" and "m" are made by shutting the
		   mouth, and the shut mouth is the shape. A longer gap is the speaker stopping, and
		   that is rest. Before this existed the closed shape never appeared on any speech,
		   which is why a mouth driven only by loudness reads as flapping -- every
		   consonant looked like the end of a sentence.

		   Either way this ignores the hold. A mouth left hanging open after the audio
		   stopped is the most obvious tell that a rig is not really listening. */
		enum ff_viseme want = (s->spoke && s->silent_ms < p->closure_ms) ? FF_VIS_A
									       : FF_VIS_X;
		if (s->current != want) {
			s->current = want;
			s->held_ms = 0.0f;
		}
		return s->current;
	}
	s->silent_ms = 0.0f;
	s->spoke = true;

	enum ff_viseme want = ff_viseme_classify(f, p->jaw_bias);

	/* Coming out of rest or a closure is immediate for the same reason: the first frame of a
	   word should already be the right shape. */
	if (s->current == FF_VIS_X || s->current == FF_VIS_A) {
		s->current = want;
		s->held_ms = 0.0f;
		return s->current;
	}

	/* Otherwise a shape holds. Without this the mouth changes on every frame where the audio
	   is ambiguous -- which is most of the frames between two vowels. */
	if (want != s->current && s->held_ms >= p->hold_ms) {
		s->current = want;
		s->held_ms = 0.0f;
	}
	return s->current;
}
