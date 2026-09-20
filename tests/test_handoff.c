#include "ff-test.h"
#include "ff-handoff.h"
#include <pthread.h>
#include <string.h>

static struct ff_handoff H;
static atomic_int stop = 0;

static void *writer(void *arg)
{
	(void)arg;
	struct ff_frame f; memset(&f, 0, sizeof f);
	for (uint32_t i = 1; !atomic_load(&stop); i++) {
		/* a frame whose every field encodes i, so a torn read is detectable */
		f.beat_count = i; f.level = (float)i; for (int b = 0; b < FF_BANDS; b++) f.bands[b] = (float)i;
		ff_handoff_publish(&H, &f);
	}
	return NULL;
}

int main(void)
{
	ff_handoff_init(&H);
	struct ff_frame out;
	CHECK(!ff_handoff_read(&H, &out));           /* nothing yet */
	pthread_t t; pthread_create(&t, NULL, writer, NULL);
	int torn = 0, reads = 0; uint32_t last = 0; int nonmono = 0;
	for (int i = 0; i < 200000; i++) {
		if (!ff_handoff_read(&H, &out)) continue;
		reads++;
		for (int b = 0; b < FF_BANDS; b++) if (out.bands[b] != (float)out.beat_count) { torn = 1; break; }
		if (out.level != (float)out.beat_count) torn = 1;
		if (out.beat_count < last) nonmono++;
		last = out.beat_count;
	}
	atomic_store(&stop, 1); pthread_join(t, NULL);
	CHECK(reads > 1000);
	CHECK(torn == 0);
	CHECK(nonmono == 0);
	FF_TEST_MAIN_END();
}
