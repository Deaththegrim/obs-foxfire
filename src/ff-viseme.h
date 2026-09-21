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

   So the classifier's real range is A-F and X. ff_cell() in packs/mouth folds G and H onto B and
   C for any pack that does draw them, which costs nothing and means a strip drawn for Rhubarb
   still works. */
enum ff_viseme {
	FF_VIS_A = 0, /* closed, slight pressure: P B M */
	FF_VIS_B,     /* slightly open, teeth together: K S T, and "EE" */
	FF_VIS_C,     /* open: "EH", "AE" */
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
 *     frontness  5% 0.104  25% 0.212  50% 0.292  75% 0.358  90% 0.425  95% 0.489  99% 0.612 */
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
 * 0.22 sits between them with roughly 2.5x of margin each way, which is why the modelling
 * question above does not decide it: under the cruder three-formant model the same fricatives
 * read 0.155-0.362 against vowels at 0.000, and 0.22 separates those too.
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
 * That does NOT mean these numbers are wrong. It means the fixture they were set against cannot
 * settle it, and neither can the other fixture. Settling it needs speech through a microphone,
 * which is also what the claim above about 638 recorded frames needs: nothing in this repo
 * reproduces those percentiles and the recording they came from is gone.
 *
 * Do not "fix" these by re-running them against the five-formant model. That would be swapping
 * one unvalidated fixture for another. */

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

/* What the classifier would say with no timing applied: the raw shape for this frame. Exposed so
   a test can check the CLASSIFICATION and the HOLD separately -- together they hide each other.
   Takes no params: the shape of a frame does not depend on any of them, and a parameter that is
   accepted and ignored is a lie about what the function reads. */
enum ff_viseme ff_viseme_classify(const struct ff_frame *f);

/* The shape's conventional letter, for logs and the properties panel. Never NULL. */
const char *ff_viseme_name(enum ff_viseme v);
