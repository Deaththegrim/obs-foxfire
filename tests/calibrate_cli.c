/* Measures the mouth classifier's features over real recorded speech.
 *
 * WHY THIS EXISTS. Every threshold in ff-viseme.h has two anchors: synthesised vowels fix what
 * is CORRECT, because their right answer is known by construction, and recorded speech fixes
 * what is BALANCED. The synthetic half lives in tests/test_viseme.c and runs on every build. The
 * recorded half was measured once, by hand, against a file that is no longer anywhere -- so the
 * distribution written into ff-viseme.c cannot be reproduced or checked, which is the whole
 * reason this is a committed tool and not a scratch program.
 *
 * Point it at a recording of the voice that will actually be driving the mouth:
 *
 *     calibrate_cli [--jaw-bias N] voice.wav [more.wav ...]
 *
 * --jaw-bias is the Mouth control of the same name, so the number can be FOUND from the
 * distribution rather than guessed at by watching a mouth. Run it once at 0, look at the shape
 * mix, and if one shape is taking most of the frames, re-run with a bias until it is not.
 *
 * It prints the percentiles of each feature over the frames that pass the noise gate, what
 * share of them each shape gets, and where the current thresholds sit. What to look for:
 *
 *   - a shape at 0.0% is a shape that will never appear, which is what happened when the
 *     wide-open threshold sat at 0.65 and no real voiced frame reached it. A and X are the
 *     exception and read near zero here BY DESIGN: this reports what ff_viseme_classify makes of
 *     each frame on its own, and the closed and rest shapes are decided by the timing in
 *     ff_viseme_update from the gaps between frames, which are exactly the frames skipped here;
 *   - a shape over about 40% is a mouth mostly stuck on one thing;
 *   - frication over the threshold on more than roughly a fifth of frames means the hiss test
 *     is firing on vowels -- that voice or that microphone is brighter than the model assumed.
 *
 * 16-bit PCM WAV only, any rate, any channel count (downmixed). That is what OBS records and
 * what ffmpeg writes with -c:a pcm_s16le.
 */
#include <ff-analysis.h>
#include <ff-viseme.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmpf(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

/* Walks the RIFF chunks rather than assuming a 44-byte header. A WAV carrying a LIST chunk --
   which anything that writes metadata does, ffmpeg included -- has its samples somewhere else,
   and reading from byte 44 would analyse the metadata as audio and report on noise. */
static float *load_wav(const char *path, uint32_t *sr_out, size_t *n_out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "%s: cannot open\n", path);
		return NULL;
	}
	char id[4];
	uint32_t sz;
	float *mono = NULL;
	size_t n = 0;
	uint16_t ch = 0, bits = 0, fmt = 0;
	uint32_t sr = 0;
	if (fread(id, 1, 4, fp) != 4 || memcmp(id, "RIFF", 4) || fread(&sz, 4, 1, fp) != 1 ||
	    fread(id, 1, 4, fp) != 4 || memcmp(id, "WAVE", 4)) {
		fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
		fclose(fp);
		return NULL;
	}
	while (fread(id, 1, 4, fp) == 4 && fread(&sz, 4, 1, fp) == 1) {
		if (!memcmp(id, "fmt ", 4)) {
			uint8_t b[40];
			uint32_t want = sz > sizeof b ? (uint32_t)sizeof b : sz;
			if (fread(b, 1, want, fp) != want)
				break;
			fmt = (uint16_t)(b[0] | (b[1] << 8));
			ch = (uint16_t)(b[2] | (b[3] << 8));
			sr = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) |
			     ((uint32_t)b[7] << 24);
			bits = (uint16_t)(b[14] | (b[15] << 8));
			if (sz > want)
				fseek(fp, (long)(sz - want), SEEK_CUR);
		} else if (!memcmp(id, "data", 4)) {
			if (fmt != 1 || bits != 16 || ch == 0) {
				/* Loudly, and with the numbers: a calibration run that silently
				   skipped a file would report percentiles over whatever was left
				   and look exactly like a successful run. */
				fprintf(stderr, "%s: need 16-bit PCM, got format %u at %u bits\n", path, fmt,
					bits);
				break;
			}
			size_t frames = sz / (size_t)(ch * 2);
			mono = malloc(frames * sizeof *mono);
			int16_t *row = malloc((size_t)ch * 2);
			if (!mono || !row) {
				free(mono);
				free(row);
				mono = NULL;
				break;
			}
			for (size_t i = 0; i < frames; i++) {
				if (fread(row, 2, ch, fp) != ch) {
					frames = i;
					break;
				}
				float s = 0.0f;
				for (int c = 0; c < ch; c++)
					s += row[c] / 32768.0f;
				mono[i] = s / (float)ch;
			}
			free(row);
			n = frames;
			break;
		} else {
			fseek(fp, (long)(sz + (sz & 1)), SEEK_CUR); /* chunks are word-aligned */
		}
	}
	fclose(fp);
	if (!mono)
		fprintf(stderr, "%s: no samples found\n", path);
	*sr_out = sr;
	*n_out = n;
	return mono;
}

#define CAP 500000
static float openness[CAP], frontness[CAP], frication[CAP], level[CAP];

static void percentiles(const char *name, float *v, size_t k, float lo_mark, float hi_mark)
{
	qsort(v, k, sizeof *v, cmpf);
	static const int Q[] = {1, 5, 25, 50, 75, 90, 95, 99};
	printf("  %-10s", name);
	for (size_t i = 0; i < sizeof Q / sizeof Q[0]; i++)
		printf(" %2d%%=%.3f", Q[i], v[k * (size_t)Q[i] / 100]);
	if (hi_mark > 0.0f)
		printf("   [thresholds %.2f / %.2f]", lo_mark, hi_mark);
	else if (lo_mark > 0.0f)
		printf("   [threshold %.2f]", lo_mark);
	printf("\n");
}

int main(int argc, char **argv)
{
	struct ff_viseme_params p;
	ff_viseme_defaults(&p);
	/* Scanned anywhere in the arguments, not just at the front. Recognised only at argv[1],
	   `calibrate_cli voice.wav --jaw-bias 0.1` treated both as filenames, failed to open them
	   on stderr, and printed a perfectly normal report at a bias of zero -- a run at the wrong
	   trim that looks right, which is the costly outcome for a tool whose purpose is finding
	   that number. */
	const char *files_argv[64];
	size_t nfiles = 0;
	for (int a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--jaw-bias")) {
			if (a + 1 >= argc) {
				fprintf(stderr, "--jaw-bias needs a number\n");
				return 2;
			}
			char *end = NULL;
			double v = strtod(argv[++a], &end);
			if (end == argv[a] || *end) {
				fprintf(stderr, "--jaw-bias: '%s' is not a number\n", argv[a]);
				return 2;
			}
			p.jaw_bias = (float)v;
		} else if (argv[a][0] == '-' && argv[a][1] == '-') {
			fprintf(stderr, "unknown option '%s'\n", argv[a]);
			return 2;
		} else if (nfiles < sizeof files_argv / sizeof files_argv[0]) {
			files_argv[nfiles++] = argv[a];
		} else {
			fprintf(stderr, "too many files (max %zu)\n", sizeof files_argv / sizeof files_argv[0]);
			return 2;
		}
	}
	if (!nfiles) {
		fprintf(stderr, "usage: %s [--jaw-bias N] voice.wav [more.wav ...]\n", argv[0]);
		return 2;
	}
	size_t k = 0, total = 0, files = 0, skipped = 0, capped = 0;
	int shape[FF_VISEME_COUNT];
	memset(shape, 0, sizeof shape);

	for (size_t a = 0; a < nfiles; a++) {
		uint32_t sr = 0;
		size_t n = 0;
		float *m = load_wav(files_argv[a], &sr, &n);
		if (!m || !n || !sr) {
			/* counted, because the diagnostics above go to stderr and the report goes
			   to stdout: redirected to a file, a run that read one of ten files looked
			   exactly like a clean one. The only signal was "from 1 file(s)", which
			   asks the reader to count their own arguments. */
			if (m && !n)
				fprintf(stderr, "%s: no samples in the data chunk\n", files_argv[a]);
			free(m);
			skipped++;
			continue;
		}
		files++;
		printf("%s: %u Hz, %.1f s\n", files_argv[a], sr, (double)n / sr);
		struct ff_analysis *an = ff_analysis_create(sr);
		for (size_t off = 0; off + FF_HOP / 2 <= n; off += FF_HOP / 2) {
			struct ff_frame f;
			if (!ff_analysis_push(an, m + off, FF_HOP / 2, &f))
				continue;
			total++;
			if (f.level <= p.gate)
				continue;
			if (k >= CAP) {
				/* NOT folded into the gate. Fused, the headline percentage silently
				   became fiction on any long recording -- and worse, the percentiles
				   and the shape mix describe a PREFIX of the audio rather than a
				   sample of it, which on a multi-file run means the first speaker
				   reported as if they were all of them. */
				capped++;
				continue;
			}
			/* All three through the engine's own accessors. This file used to re-sum
			   the bands with its own copy of the four window edges, so moving one in
			   ff-viseme.c would have left this reporting the old distribution against
			   the new thresholds -- in the one tool whose entire job is saying whether
			   a threshold is balanced against the feature the classifier computes. */
			openness[k] = ff_viseme_openness(&f);
			frontness[k] = ff_viseme_frontness(&f);
			frication[k] = ff_viseme_frication(&f);
			level[k] = f.level;
			shape[ff_viseme_classify(&f, p.jaw_bias)]++;
			k++;
		}
		ff_analysis_destroy(an);
		free(m);
	}

	if (!files) {
		fprintf(stderr, "nothing was read -- no percentiles to report\n");
		return 2;
	}
	if (!k) {
		fprintf(stderr, "%zu frames, none over the %.3f noise gate -- too quiet to calibrate\n", total,
			(double)p.gate);
		return 2;
	}
	/* The denominator, always: a distribution over 40 frames is not a calibration, and the
	   only way to know that is for the count to be printed next to it. */
	printf("\n%zu voiced frames of %zu (%.0f%% over the %.3f gate), from %zu file(s), jaw bias %+.3f\n",
	       k + capped, total, 100.0 * (double)(k + capped) / (double)total, (double)p.gate, files,
	       (double)p.jaw_bias);
	if (skipped)
		printf("  SKIPPED %zu of %zu input file(s) -- see stderr. What follows describes the "
		       "rest.\n",
		       skipped, skipped + files);
	if (capped)
		printf("  CAPPED: %zu voiced frame(s) past the first %d were not measured, so what "
		       "follows is the START of this audio and not a sample of it.\n",
		       capped, CAP);
	printf("  shape mix:");
	for (int i = 0; i < FF_VISEME_COUNT; i++)
		printf(" %s=%.1f%%", ff_viseme_name(i), 100.0 * shape[i] / (double)k);
	printf("   (A and X come from the timing, not from here -- near 0%% is correct)\n");
	percentiles("openness", openness, k, FF_VIS_JAW - p.jaw_bias, FF_VIS_OPEN_WIDE - p.jaw_bias);
	percentiles("frontness", frontness, k, FF_VIS_FRONT, 0.0f);
	percentiles("frication", frication, k, FF_VIS_FRICATION, 0.0f);
	percentiles("level", level, k, p.gate, 0.0f);
	if (k < 500)
		printf("\n  NOTE: under 500 voiced frames (about 10 seconds of talking). Enough to spot a\n"
		       "  shape that never fires; not enough to move a threshold on.\n");
	/* A calibration over some of the input is not a calibration, and the exit code is the only
	   part of this a script reads. */
	return skipped ? 1 : 0;
}
