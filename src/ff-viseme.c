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

/* Total energy between two frequencies, by summing the bands whose centres fall inside. */
static float band_energy(const struct ff_frame *f, float lo_hz, float hi_hz)
{
	float sum = 0.0f;
	for (int i = 0; i < FF_BANDS; i++) {
		float hz = ff_viseme_band_hz(i);
		if (hz >= lo_hz && hz <= hi_hz)
			sum += f->bands[i];
	}
	return sum;
}

enum ff_viseme ff_viseme_classify(const struct ff_frame *f)
{
	/* Voiced energy only. Everything above ~3.5 kHz is sibilance and room noise, and below
	   ~180 Hz is the fundamental and whatever the desk is resting on; neither says anything
	   about the shape of the mouth. */
	float voiced = band_energy(f, 180.0f, 3500.0f);
	if (voiced <= 0.0001f)
		return FF_VIS_X;

	/* JAW: where the energy sits inside F1's range. The low half of the range is a closed jaw
	   ("ee", "oo" at 240-250 Hz), the top is wide open ("ah" at 850). A ratio rather than a
	   peak, because a peak needs to be told which formant it found and a ratio does not. */
	float f1_low = band_energy(f, 180.0f, 450.0f);
	float f1_high = band_energy(f, 450.0f, 1100.0f);
	float openness = f1_high / (f1_low + f1_high + 1e-6f);

	/* FRONT vs BACK: F2 is up at 2300-2400 Hz for front vowels and down at ~600 Hz for back,
	   rounded ones. The gap is enormous, which is what makes this robust at 10% band spacing.
	   The low window starts above F1's range so a wide-open "ah" does not read as rounded. */
	float front = band_energy(f, 1800.0f, 3000.0f);
	float back = band_energy(f, 550.0f, 1300.0f);
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
	 * out wrong there is wrong. Real speech fixes what is BALANCED -- measured over 638 voiced
	 * frames of recorded voice:
	 *
	 *     openness   5% 0.281  25% 0.392  50% 0.469  75% 0.521  90% 0.564  99% 0.630
	 *     frontness  5% 0.104  25% 0.212  50% 0.292  75% 0.358  90% 0.425  99% 0.612
	 *
	 * The first version put "wide open" at 0.65, which 99% of real frames never reach -- so D
	 * never appeared at all, on any speech. Synthetic vowels alone would never have shown
	 * that: "ae" hits 0.776 and classified perfectly. A threshold can be right about the
	 * physics and still be wrong about a voice. */
	if (openness > FF_VIS_OPEN_WIDE)
		return FF_VIS_D; /* wide open: "ah" is a wide mouth whatever the tongue is doing,
				    and deciding front-or-back at that jaw angle makes a held vowel
				    twitch between two shapes */

	bool is_front = frontness > FF_VIS_FRONT;
	if (openness > FF_VIS_JAW)
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

	enum ff_viseme want = ff_viseme_classify(f);

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
