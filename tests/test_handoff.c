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

/* Read until enough reads have LANDED, not until a fixed iteration count -- with a deadline
   so a handoff that never publishes still fails instead of spinning forever.
   `reads > MIN_READS` is the arming check: torn and nonmono below are both vacuously true
   if nothing was ever read, so this is what makes them mean something. But a fixed 200000
   attempts assumes the reader wins the race often enough, and on a 2-core CI runner sharing
   those cores with the writer it did not: 200000 attempts, under 1000 landed, and the test
   failed for being descheduled rather than for anything about the seqlock. The guarantees
   this file exists to check are torn == 0 and nonmono == 0, and neither depends on how many
   attempts it took to collect the evidence. */
#define MIN_READS 1000
#define LIMIT_S 20.0

struct reader_result {
	int torn;    /* a frame whose fields disagree: the writer was seen mid-update */
	int nonmono; /* beat_count went backwards: an older slot was served after a newer one */
	int reads;   /* how many reads LANDED -- without this the two above prove nothing */
};

/* One reader, with its own `out`. Shared by both phases below rather than written twice: the
   torn-frame check is the substance of this file, and a second copy of it is a second thing to
   keep correct. */
static void *reader(void *p)
{
	struct reader_result *r = p;
	struct ff_frame out;
	uint32_t last = 0;
	int attempts = 0;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (r->reads < MIN_READS * 2) {
		if (++attempts % 4096 == 0) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			double secs = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
			if (secs > LIMIT_S)
				break;
		}
		if (!ff_handoff_read(&H, &out))
			continue;
		r->reads++;
		for (int b = 0; b < FF_BANDS; b++)
			if (out.bands[b] != (float)out.beat_count) {
				r->torn = 1;
				break;
			}
		if (out.level != (float)out.beat_count)
			r->torn = 1;
		if (out.beat_count < last)
			r->nonmono++;
		last = out.beat_count;
	}
	return NULL;
}

/* Four, not two: two readers can interleave with the writer in a way that never puts two reads
   inside the same publish window, and the property under test is precisely what happens when they
   do. Four on any ordinary machine oversubscribes the cores the writer is also on, which is the
   condition that makes the overlap happen rather than hoping for it. */
#define N_READERS 4

int main(void)
{
	ff_handoff_init(&H);
	struct ff_frame out;
	CHECK(!ff_handoff_read(&H, &out)); /* nothing yet */

	/* Phase 1: the original single reader against the single writer. */
	pthread_t t;
	atomic_store(&stop, 0);
	pthread_create(&t, NULL, writer, NULL);
	struct reader_result one = {0, 0, 0};
	reader(&one);
	atomic_store(&stop, 1);
	pthread_join(t, NULL);
	CHECK(one.reads > MIN_READS);
	CHECK(one.torn == 0);
	CHECK(one.nonmono == 0);

	/* Phase 1b: a read must not write to the handoff AT ALL.
	   This is what actually makes several readers safe, and it is a structural property, so it is
	   checked structurally: snapshot every byte of the struct, read, and require the bytes to be
	   identical. Single-threaded on purpose -- with the writer running, everything changes and the
	   check says nothing.
	   Phase 2 below cannot catch this. Measured: staging the copy through a `scratch` frame added
	   to struct ff_handoff -- the shape a plausible "avoid a big copy on the retry path" change
	   would take -- passed all 16 of phase 1 and phase 2's checks, because every field of a test
	   frame encodes the same counter, so one reader clobbering another's staging area still yields
	   a self-consistent frame. Nor is ThreadSanitizer the answer: it reports a race on this file
	   UNMUTATED (1 on the clean source, 2 on the mutated one), because a seqlock's unsynchronised
	   slot write racing its read is the design and `seq` is what detects it. A gate that
	   distinguishes "one expected race" from "two" is not a gate. This does the job in four
	   lines and fails the moment a reader touches shared state. */
	struct ff_frame probe;
	memset(&probe, 0, sizeof probe);
	probe.beat_count = 7;
	ff_handoff_publish(&H, &probe);
	unsigned char before[sizeof(struct ff_handoff)];
	memcpy(before, &H, sizeof before);
	CHECK(ff_handoff_read(&H, &out));
	CHECK(memcmp(before, &H, sizeof before) == 0);

	/* Phase 2: SEVERAL readers at once, which is the claim the dock rests on.
	   ff_handoff_read mutates nothing in the struct -- it loads seq, copies a slot into the
	   caller's own frame, and re-reads seq to check the writer did not lap it -- so readers
	   cannot interfere with each other and the writer never waits for any of them. That is a
	   property of how it is written, and a property nobody had watched hold. Without this, "a
	   meter can poll the same handoff the render thread is reading" was an inference from the
	   source rather than something measured. Each reader keeps its own result so a single
	   misbehaving one cannot be averaged away. */
	atomic_store(&stop, 0);
	pthread_create(&t, NULL, writer, NULL);
	pthread_t rt[N_READERS];
	struct reader_result many[N_READERS];
	for (int i = 0; i < N_READERS; i++) {
		many[i] = (struct reader_result){0, 0, 0};
		pthread_create(&rt[i], NULL, reader, &many[i]);
	}
	for (int i = 0; i < N_READERS; i++)
		pthread_join(rt[i], NULL);
	atomic_store(&stop, 1);
	pthread_join(t, NULL);
	for (int i = 0; i < N_READERS; i++) {
		CHECK(many[i].reads > MIN_READS);
		CHECK(many[i].torn == 0);
		CHECK(many[i].nonmono == 0);
	}
	FF_TEST_MAIN_END();
}
