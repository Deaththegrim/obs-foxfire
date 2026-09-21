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
   order because it is the order the art is conventionally drawn and numbered in. */
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

/* jaw open enough to be a wide mouth */
#define FF_VIS_OPEN_WIDE 0.56f
/* jaw open at all, as opposed to nearly closed */
#define FF_VIS_JAW 0.48f
/* tongue forward and lips unrounded, as opposed to back and rounded */
#define FF_VIS_FRONT 0.29f

/* The centre frequency of band `i`, in Hz. Exposed because a test that hard-codes band indices
   is a test that breaks silently the day the layout changes. */
float ff_viseme_band_hz(int i);

/* What the classifier would say with no timing applied: the raw shape for this frame. Exposed so
   a test can check the CLASSIFICATION and the HOLD separately -- together they hide each other.
   Takes no params: the shape of a frame does not depend on any of them, and a parameter that is
   accepted and ignored is a lie about what the function reads. */
enum ff_viseme ff_viseme_classify(const struct ff_frame *f);

/* The shape's conventional letter, for logs and the properties panel. Never NULL. */
const char *ff_viseme_name(enum ff_viseme v);
