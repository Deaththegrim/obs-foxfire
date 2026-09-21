#include "ff-analysis.h"
#include "kiss_fftr.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLUX_HISTORY 43 /* ~1 s of hops at 48 kHz */
#define FF_XOVER_HZ 250.f /* bands entirely below this read the long-window FFT */

struct ff_analysis {
	uint32_t sr;
	struct ff_analysis_params p;
	float ring[FF_RING];
	size_t ring_pos;  /* next write */
	size_t since_hop; /* samples since last FFT */
	float window[FF_FFT], window_low[FF_FFT_LOW];
	kiss_fftr_cfg cfg, cfg_low;
	float td[FF_FFT], td_low[FF_FFT_LOW];
	kiss_fft_cpx fd[FF_FFT / 2 + 1], fd_low[FF_FFT_LOW / 2 + 1];
	int band_lo[FF_BANDS], band_hi[FF_BANDS]; /* bin ranges, inclusive-exclusive, in band_src's FFT */
	bool band_src_low[FF_BANDS];              /* true: read fd_low, not fd */
	float band_centre_hz[FF_BANDS];           /* geometric centre; the two FFTs have different bin widths,
	                                             so a centre derived from bins is only valid per source */
	float band_raw[FF_BANDS];
	float prev_low[FF_BANDS];
	float flux_hist[FLUX_HISTORY];
	int flux_pos;
	int refractory_hops;
	float hop_seconds;
	struct ff_frame f;
};

static float db_to_unit(float v) /* 0..1 from linear amplitude, -60 dB floor */
{
	if (v <= 1e-6f)
		return 0.f;
	float db = 20.f * log10f(v);
	float u = (db + 60.f) / 60.f;
	return u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
}

struct ff_analysis *ff_analysis_create(uint32_t sample_rate)
{
	struct ff_analysis *a = calloc(1, sizeof *a);
	a->sr = sample_rate ? sample_rate : 48000;
	a->cfg = kiss_fftr_alloc(FF_FFT, 0, NULL, NULL);
	a->cfg_low = kiss_fftr_alloc(FF_FFT_LOW, 0, NULL, NULL);
	for (int i = 0; i < FF_FFT; i++)
		a->window[i] = 0.5f - 0.5f * cosf(2.f * 3.14159265f * (float)i / (float)(FF_FFT - 1));
	for (int i = 0; i < FF_FFT_LOW; i++)
		a->window_low[i] = 0.5f - 0.5f * cosf(2.f * 3.14159265f * (float)i / (float)(FF_FFT_LOW - 1));
	const float fmin = 30.f, fmax = 16000.f;
	const float binhz_hi = (float)a->sr / (float)FF_FFT, binhz_lo = (float)a->sr / (float)FF_FFT_LOW;
	for (int b = 0; b < FF_BANDS; b++) {
		float lo = fmin * powf(fmax / fmin, (float)b / FF_BANDS),
		      hi = fmin * powf(fmax / fmin, (float)(b + 1) / FF_BANDS);
		/* geometric, not arithmetic: these bands are log-spaced, so the arithmetic midpoint
		   sits above the band's true centre and skews the bass/mid/treble split. */
		a->band_centre_hz[b] = sqrtf(lo * hi);
		/* the whole band must fit below the crossover -- a band straddling it would take its
		   width from one FFT and its content from the other */
		bool low = hi <= FF_XOVER_HZ;
		float binhz = low ? binhz_lo : binhz_hi;
		int nbins = low ? FF_FFT_LOW / 2 : FF_FFT / 2;
		int blo = (int)(lo / binhz), bhi = (int)(hi / binhz);
		if (bhi <= blo)
			bhi = blo + 1;
		if (bhi > nbins)
			bhi = nbins;
		a->band_src_low[b] = low;
		a->band_lo[b] = blo;
		a->band_hi[b] = bhi;
	}
	a->p.release_ms = 150.f;
	a->p.beat_sensitivity = 1.f;
	a->p.gain_db = 0.f;
	a->hop_seconds = (float)FF_HOP / (float)a->sr;
	return a;
}

void ff_analysis_destroy(struct ff_analysis *a)
{
	if (!a)
		return;
	kiss_fftr_free(a->cfg);
	kiss_fftr_free(a->cfg_low);
	free(a);
}
void ff_analysis_set_params(struct ff_analysis *a, const struct ff_analysis_params *p)
{
	a->p = *p;
}

static void analyse(struct ff_analysis *a)
{
	const float gain = powf(10.f, a->p.gain_db / 20.f);
	/* Fast window: the most recent FF_FFT samples of the ring. The ring is FF_RING long now, so
	   "oldest first" for the SHORT window starts FF_FFT back from the write head, not at it. */
	float sumsq = 0.f, pk = 0.f;
	const size_t start = (a->ring_pos + FF_RING - FF_FFT) % FF_RING;
	for (int i = 0; i < FF_FFT; i++) {
		float s = a->ring[(start + (size_t)i) % FF_RING] * gain;
		if (s > 1.f)
			s = 1.f;
		if (s < -1.f)
			s = -1.f;
		a->td[i] = s * a->window[i];
		if (i >= FF_FFT - FF_HOP) {
			sumsq += s * s;
			float m = fabsf(s);
			if (m > pk)
				pk = m;
		}
	}
	/* Long window: the whole ring, oldest first. Only the bands below FF_XOVER_HZ read it. */
	for (int i = 0; i < FF_FFT_LOW; i++) {
		float s = a->ring[(a->ring_pos + (size_t)i) % FF_RING] * gain;
		if (s > 1.f)
			s = 1.f;
		if (s < -1.f)
			s = -1.f;
		a->td_low[i] = s * a->window_low[i];
	}
	kiss_fftr(a->cfg, a->td, a->fd);
	kiss_fftr(a->cfg_low, a->td_low, a->fd_low);
	const float release = expf(-a->hop_seconds / (a->p.release_ms / 1000.f + 1e-3f));
	for (int b = 0; b < FF_BANDS; b++) {
		const kiss_fft_cpx *spec = a->band_src_low[b] ? a->fd_low : a->fd;
		float maxpow = 0.f;
		for (int k = a->band_lo[b]; k < a->band_hi[b]; k++) {
			float pw = spec[k].r * spec[k].r + spec[k].i * spec[k].i;
			if (pw > maxpow)
				maxpow = pw;
		}
		/* peak bin, not band-mean: a sine at 0 dBFS lights its band to ~1.0 regardless of band width.
		   The Hann coherent-gain compensation is 4/N for THIS band's transform -- using the wrong N
		   would put a step in the spectrum exactly at the crossover. */
		float amp = sqrtf(maxpow) * (4.f / (float)(a->band_src_low[b] ? FF_FFT_LOW : FF_FFT));
		float u = db_to_unit(amp);
		a->band_raw[b] = u;
		a->f.bands[b] = u > a->f.bands[b] ? u : a->f.bands[b] * release;
		if (a->f.bands[b] < 1e-4f)
			a->f.bands[b] = 0.f;
		a->f.peaks[b] = u > a->f.peaks[b] ? u : a->f.peaks[b] - 0.6f * a->hop_seconds;
		if (a->f.peaks[b] < 0.f)
			a->f.peaks[b] = 0.f;
	}
	/* level / peak */
	float rms = sqrtf(sumsq / (float)FF_HOP);
	float lrel = expf(-a->hop_seconds / 0.1f);
	a->f.level = rms > a->f.level ? rms : a->f.level * lrel;
	if (a->f.level < 1e-4f)
		a->f.level = 0.f;
	a->f.peak = pk > a->f.peak ? pk : a->f.peak * release;
	if (a->f.peak < 1e-4f)
		a->f.peak = 0.f;
	/* bass / mid / treble by frequency */
	float bs = 0, ms = 0, ts = 0;
	int bn = 0, mn = 0, tn = 0;
	for (int b = 0; b < FF_BANDS; b++) {
		float centre = a->band_centre_hz[b];
		if (centre < 250.f) {
			bs += a->f.bands[b];
			bn++;
		} else if (centre < 4000.f) {
			ms += a->f.bands[b];
			mn++;
		} else {
			ts += a->f.bands[b];
			tn++;
		}
	}
	a->f.bass = bn ? bs / bn : 0.f;
	a->f.mid = mn ? ms / mn : 0.f;
	a->f.treble = tn ? ts / tn : 0.f;
	/* beat: spectral flux on bands < 4 kHz */
	float flux = 0.f;
	for (int b = 0; b < FF_BANDS; b++) {
		float centre = a->band_centre_hz[b];
		if (centre >= 4000.f)
			break;
		float d = a->band_raw[b] - a->prev_low[b];
		if (d > 0.f)
			flux += d;
		a->prev_low[b] = a->band_raw[b];
	}
	float mean = 0.f, var = 0.f;
	for (int i = 0; i < FLUX_HISTORY; i++)
		mean += a->flux_hist[i];
	mean /= FLUX_HISTORY;
	for (int i = 0; i < FLUX_HISTORY; i++) {
		float d = a->flux_hist[i] - mean;
		var += d * d;
	}
	float sd = sqrtf(var / FLUX_HISTORY);
	float k = 1.5f / (a->p.beat_sensitivity > 0.05f ? a->p.beat_sensitivity : 0.05f);
	bool onset = a->refractory_hops == 0 && flux > 0.02f && flux > mean + k * sd;
	a->flux_hist[a->flux_pos] = flux;
	a->flux_pos = (a->flux_pos + 1) % FLUX_HISTORY;
	if (a->refractory_hops > 0)
		a->refractory_hops--;
	const float beat_decay = expf(-a->hop_seconds / 0.2886f); /* 200 ms half-life */
	if (onset) {
		a->f.beat = 1.f;
		a->f.beat_count++;
		a->refractory_hops = 5; /* ~107 ms */
	} else {
		a->f.beat *= beat_decay;
		if (a->f.beat < 0.02f)
			a->f.beat = 0.f;
	} /* snap an imperceptible envelope tail to exactly 0 */
	/* waveform: last 1024 samples decimated by 2 */
	const size_t wstart = (a->ring_pos + FF_RING - FF_HOP) % FF_RING;
	for (int i = 0; i < FF_WAVE; i++) {
		float s = a->ring[(wstart + (size_t)(2 * i)) % FF_RING] * gain;
		a->f.wave[i] = s > 1.f ? 1.f : (s < -1.f ? -1.f : s);
	}
}

bool ff_analysis_push(struct ff_analysis *a, const float *mono, size_t frames, struct ff_frame *out)
{
	bool updated = false;
	for (size_t i = 0; i < frames; i++) {
		a->ring[a->ring_pos] = mono[i];
		a->ring_pos = (a->ring_pos + 1) % FF_RING;
		if (++a->since_hop >= FF_HOP) {
			a->since_hop = 0;
			analyse(a);
			updated = true;
		}
	}
	if (updated)
		*out = a->f;
	return updated;
}
