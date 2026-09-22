#include "ff-test.h"
#include "ff-handoff.h"
#include <pthread.h>
#include <string.h>
#include <time.h>

static struct ff_handoff H;
static atomic_int stop = 0;

static void *writer(void *arg)
{
	(void)arg;
	struct ff_frame f;
	memset(&f, 0, sizeof f);
	for (uint32_t i = 1; !atomic_load(&stop); i++) {
		/* a frame whose every field encodes i, so a torn read is detectable */
		f.beat_count = i;
		f.level = (float)i;
		for (int b = 0; b < FF_BANDS; b++)
			f.bands[b] = (float)i;
		ff_handoff_publish(&H, &f);
	}
	return NULL;
}

int main(void)
{
	ff_handoff_init(&H);
	struct ff_frame out;
	CHECK(!ff_handoff_read(&H, &out)); /* nothing yet */
	pthread_t t;
	pthread_create(&t, NULL, writer, NULL);
	int torn = 0, reads = 0;
	uint32_t last = 0;
	int nonmono = 0;
	/* Read until enough reads have LANDED, not until a fixed iteration count -- with a deadline
	   so a handoff that never publishes still fails instead of spinning forever.
	   `reads > MIN_READS` is the arming check: torn and nonmono below are both vacuously true
	   if nothing was ever read, so this is what makes them mean something. But a fixed 200000
	   attempts assumes the reader wins the race often enough, and on a 2-core CI runner sharing
	   those cores with the writer it did not: 200000 attempts, under 1000 landed, and the test
	   failed for being descheduled rather than for anything about the seqlock. The guarantees
	   this file exists to check are torn == 0 and nonmono == 0, and neither depends on how many
	   attempts it took to collect the evidence. */
	const int MIN_READS = 1000;
	const double LIMIT_S = 20.0;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int attempts = 0;
	while (reads < MIN_READS * 2) {
		if (++attempts % 4096 == 0) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double secs = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
			if (secs > LIMIT_S)
				break;
		}
		if (!ff_handoff_read(&H, &out))
			continue;
		reads++;
		for (int b = 0; b < FF_BANDS; b++)
			if (out.bands[b] != (float)out.beat_count) {
				torn = 1;
				break;
			}
		if (out.level != (float)out.beat_count)
			torn = 1;
		if (out.beat_count < last)
			nonmono++;
		last = out.beat_count;
	}
	atomic_store(&stop, 1);
	pthread_join(t, NULL);
	CHECK(reads > MIN_READS);
	CHECK(torn == 0);
	CHECK(nonmono == 0);
	FF_TEST_MAIN_END();
}
