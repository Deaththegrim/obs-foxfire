#pragma once
#include "ff-frame.h"

#include <stdbool.h>

/* Mouth shapes from audio -- lipsync, for a character overlay.
 *
 * WHY THIS IS NOT A SHADER. The thing that separates lipsync that reads as speech from a mouth
 * that flaps is not the classification, it is the TIMING: a shape has to hold long enough to be
 * seen, and must not flicker between two neighbours on a frame where the audio is ambiguous. That
 * needs memory of what the mouth was doing last frame, and an OBS shader has none. So the shape
 * is decided here and handed to the shader as a number.
 *
 * WHAT IT KEYS ON. Not loudness -- loudness only tells you the mouth is open, which is why
 * amplitude-only mouths make every syllable look identical. Vowel identity lives in the first two
 * formants (Wikipedia, "Formant", measured for male speakers):
 *
 *     vowel        F1      F2       jaw        tongue/lips
 *     i "fleece"   240    2400      closed     front
 *     e "dress"    390    2300      mid        front
 *     a "trap"     850    1610      open       central
 *     o "thought"  360     640      mid        back, rounded
 *     u "goose"    250     595      closed     back, rounded
 *
 * F1 rises as the jaw opens; F2 falls as the tongue moves back and the lips round. That is a 2D
 * space, and the classic mouth-shape set maps onto it directly -- which is why this works at all
 * without recognising a single phoneme.
 *
 * The engine's 64 bands are logarithmic from 30 Hz to 16 kHz, about 10% per band, so F1's range
 * spans ~13 bands and F2's ~15. Coarse, but the classes are far apart: "ee" at 2400 Hz and "oo"
 * at 595 Hz are nine bands apart. Measured rather than assumed -- see tests/test_viseme.c, which
 * synthesises vowels at those exact formants and checks the shape that comes out.
 *
 * Tracking F1 and F2 as peaks is the obvious approach and is NOT what this does: their ranges
 * overlap (F1 reaches 850, F2 starts at 595), so a back vowel puts both in the same window and
 * peak-picking mixes them up. Two band-energy ratios separate the same classes without ever
 * having to decide which peak is which.
 */

/* The Preston Blair set, as used by Rhubarb Lip Sync. A-F are the ones that matter; G, H and X
   are optional and an artist may skip them (rhubarb's own documentation says so). Kept in this
   order because it is the order the art is conventionally drawn and numbered in.

   G AND H ARE NEVER RETURNED, and an artist should not spend a cell on either. Both are
   articulatory facts rather than spectral ones: G is the teeth on the lip in F and V, and telling
   /f/ from /s/ comes down to amplitude and how sharp the spectral peak is -- /f/ is flat and some
   15-20 dB quieter -- neither of which survives 64 logarithmic bands and a microphone whose gain
   is a knob on the desk. H is the tongue up in a long L, which is an ANTI-formant, a notch, and
   nothing here looks for notches. Rhubarb produces them because it runs a phoneme recogniser over
   the whole file offline; doing that live would mean shipping an acoustic model and spending the
   CPU on a machine that is already encoding video. X is returned, and is the rest position.

   So the classifier's real range is A-F and X. A strip drawn for Rhubarb still works: its G and
   H cells are simply never indexed, since nothing ever asks for them. ff_cell() in packs/mouth
   folds indices the strip does NOT have onto the drawn cell with the nearest mouth aperture,
   which is what lets a three- or four-cell strip work at all -- and it produces Rhubarb's own
   X->A, G->B and H->C as a side effect rather than by special-casing them. (This used to say
   ff_cell folds G and H for packs that DO draw them, which is backwards: it folds only what is
   missing.) */
enum ff_viseme {
	FF_VIS_A = 0, /* closed, slight pressure: P B M */
	FF_VIS_B,     /* slightly open, teeth together: K S T, and "EE" */
	FF_VIS_C,     /* open: "EH". NOT "AE" -- at F1 850 that measures openness 0.776 and
	                 classifies as D, which tests/test_viseme.c asserts. */
	FF_VIS_D,     /* wide open: "AA" as in father */
	FF_VIS_E,     /* slightly rounded: "AO", "ER" */
	FF_VIS_F,     /* puckered: "UW", "OW", "W" */
	FF_VIS_G,     /* teeth on lip: F V  (optional) */
	FF_VIS_H,     /* tongue up: long L   (optional) */
	FF_VIS_X,     /* rest, relaxed closed */
	FF_VISEME_COUNT,
};

struct ff_viseme_params {
	/* Below this level the mouth is at rest. A noise gate, and it matters: breath, a keyboard
	   and a fan all have energy, and a mouth that answers them looks possessed. */
	float gate;
	/* A gap SHORTER than this is a stop consonant -- the closure in "p", "b", "m", "t" -- and
	   the mouth should be SHUT, not resting. Longer, and the speaker has stopped. Without the
	   distinction the closed shape never appears on speech at all, and closures are most of
	   what makes lipsync read as talking rather than as a slideshow of vowels. */
	float closure_ms;
	/* Minimum time a shape stays up once chosen, milliseconds. The single most important
	   number here -- without it the mouth changes every frame and reads as flapping rather
	   than speech. Around 70-90 ms is roughly the length of a spoken phoneme. */
	float hold_ms;
	/* How fast openness follows the audio, milliseconds to close. Too fast and the mouth
	   chatters between words; too slow and it hangs open. */
	float release_ms;
	/* Shifts BOTH jaw thresholds together. Positive opens the mouth more readily, negative
	   keeps it shut.

	   This exists because openness turned out not to be portable between recordings, and the
	   measurement is in ff-viseme.c: across four real corpora its median runs from 0.433 to
	   0.589 -- a spread of 0.156, which is nearly twice the gap between the two thresholds it
	   is compared against. One of the four sits on the wide-open shape 56% of the time. The
	   cause is the recording as much as the speaker, because this feature is the balance of two
	   bands and anything that tilts the low end tilts it.

	   Nothing else in this struct touches that: the gate decides whether to listen, and the
	   three times decide when a shape may change, but none of them can tell a mouth that hangs
	   open to stop. Without this the only fix would be a rebuild. */
	float jaw_bias;
};

void ff_viseme_defaults(struct ff_viseme_params *p);

struct ff_viseme_state {
	enum ff_viseme current;
	float held_ms;   /* how long `current` has been up */
	float openness;  /* smoothed 0..1, exposed so a shader can blend between shapes */
	float silent_ms; /* how long the level has been under the gate */
	bool spoke;      /* anything has been said yet. A mouth that has never heard speech rests
			    rather than snapping shut, so a scene loaded in silence looks idle. */
	bool voiced;
};

void ff_viseme_init(struct ff_viseme_state *s);

/* Advances by `dt_ms` using this frame's spectrum. Returns the shape to draw. */
enum ff_viseme ff_viseme_update(struct ff_viseme_state *s, const struct ff_frame *f, float dt_ms,
				const struct ff_viseme_params *p);

/* The decision boundaries, named so a test can check them against measured speech rather than
 * only against synthetic vowels. That distinction bit once already: the wide-open threshold was
 * 0.65, every synthetic vowel classified perfectly, and the shape never appeared on a real voice
 * in twelve seconds of talking. A constant can be right about the physics and wrong about people.
 *
 * Measured over 638 voiced frames of recorded speech:
 *     openness   5% 0.281  25% 0.392  50% 0.469  75% 0.521  90% 0.564  95% 0.588  99% 0.630
 *     frontness  5% 0.104  25% 0.212  50% 0.292  75% 0.358  90% 0.425  95% 0.489  99% 0.612
 *
 * THAT recording is gone and these four numbers are the older anchor, kept because the four
 * corpora measured since (see ff-viseme.c) agree with them: medians of 0.440/0.433/0.484/0.589
 * against this 0.469, and a frontness median within 0.05 on every one. They are not re-derived
 * from the newer corpora because the constants they justify have not moved. */
#define FF_VIS_P50_OPEN 0.469f
#define FF_VIS_P90_OPEN 0.564f
#define FF_VIS_P25_FRONT 0.212f
#define FF_VIS_P75_FRONT 0.358f

/* Share of 180 Hz-16 kHz energy sitting above 3.5 kHz. Over this and the frame is frication --
 * a hiss, not a vowel -- and the two ratios above mean nothing on it.
 *
 * Measured through this engine's own analysis, with the vowels synthesised through FIVE formants
 * and a lip-radiation term. That detail is the whole measurement: three resonators roll off
 * 36 dB/octave above F3, so a three-formant vowel has literally nothing over 3.5 kHz, reads 0.000
 * however breathy you make it, and would put this threshold anywhere at all. A real tract has
 * formants near 3.5 and 4.5 kHz and radiates from the lips at +6 dB/octave.
 *
 *     vowels, clean to aspiration -6 dB     0.000 - 0.087   (worst: a breathy "eh")
 *     fricatives                            0.418 - 0.705   (worst: a dark /sh/ peaking at 2.6k)
 *
 * 0.22 sits between them with 2.5x of margin below and 1.9x above -- the test asserts only the
 * 1.6x it can guarantee. The modelling question above does not decide it either: under the
 * cruder three-formant model the same fricatives read 0.155-0.362 against vowels at 0.000, and
 * 0.22 separates all but the weakest of those. (This claimed "roughly 2.5x each way" and that
 * 0.22 separates the 0.155 case, which is below it.)
 *
 * NOT YET CHECKED AGAINST A REAL MICROPHONE. Both anchors here are synthetic, and the upper one
 * is the one that matters: a bright mic, a sibilant voice or no de-esser all push real vowels up,
 * and nothing here would notice. Erring HIGH is deliberate -- a missed fricative leaves the old
 * behaviour, a false one puts the teeth together on a vowel and is visible immediately. */
#define FF_VIS_FRICATION 0.22f

/* KNOWN UNFINISHED, and the frication work above is what turned it up. The three vowel
 * thresholds below were set against a THREE-formant synthesis with no lip radiation, which has
 * nothing above 3.5 kHz. Run the same five vowels through the five-formant model with radiation
 * -- the realistic one -- and "ae" still lands on D at every breathiness, but "eh" moves between
 * B, C and D and "aw" between D, E and F. The tighter axis is openness, and radiation tilts the
 * spectrum, which moves it.
 *
 * That does NOT mean these numbers are wrong, and real speech has since said they are not: over
 * 107,259 voiced frames of four recorded voices every shape from B to F fires, worst case 7.6%,
 * which is the check the synthetic fixtures cannot perform. The full distribution is in
 * ff-viseme.c. What the same measurement DID find is that openness is not portable between
 * recordings, which is what jaw_bias is for.
 *
 * Still open: all four corpora are somebody else's voice, and none is a streaming microphone in
 * the room the mouth will actually run in. Do not "fix" these by re-running them against the
 * five-formant model -- that would be swapping one unvalidated fixture for another. */

/* How far the jaw trim may go. Defined beside the thresholds it shifts, and used BOTH by the
   panel slider and by the clamp in ff_viseme_classify, so the bound the classifier enforces and
   the bound the panel offers cannot drift apart. */
#define FF_VIS_JAW_BIAS_MAX 0.15f

/* jaw open enough to be a wide mouth */
#define FF_VIS_OPEN_WIDE 0.56f
/* jaw open at all, as opposed to nearly closed */
#define FF_VIS_JAW 0.48f
/* tongue forward and lips unrounded, as opposed to back and rounded */
#define FF_VIS_FRONT 0.29f

/* The centre frequency of band `i`, in Hz. Exposed because a test that hard-codes band indices
   is a test that breaks silently the day the layout changes. */
float ff_viseme_band_hz(int i);

/* The high-band share this frame, 0..1 -- the feature FF_VIS_FRICATION is compared against.
   Exposed for the same reason as the band layout: a test that can only see the shape that came
   out cannot tell a threshold that is wrong from a feature that is wrong, and "ee" classifies as
   B whether it was heard as a vowel or as a hiss. */
float ff_viseme_frication(const struct ff_frame *f);

/* The two vowel features, on the same terms: the jaw axis and the front/back axis this frame,
   each compared against the thresholds above. A caller measuring a distribution must read these
   rather than re-summing the bands, or it ends up reporting one feature's percentiles against
   another feature's thresholds and nothing says so. */
float ff_viseme_openness(const struct ff_frame *f);
float ff_viseme_frontness(const struct ff_frame *f);

/* What the classifier would say with no timing applied: the raw shape for this frame. Exposed so
   a test can check the CLASSIFICATION and the HOLD separately -- together they hide each other.

   Takes the jaw bias, and nothing else. The old rule has not changed -- a parameter that is
   accepted and ignored is a lie about what the function reads -- and this is the only setting
   that changes the answer for a single frame. The gate and the three timings are all about what
   happens BETWEEN frames and live in ff_viseme_update. Pass 0 for the untrimmed answer. */
enum ff_viseme ff_viseme_classify(const struct ff_frame *f, float jaw_bias);

/* The shape's conventional letter, for logs and the properties panel. Never NULL. */
const char *ff_viseme_name(enum ff_viseme v);
