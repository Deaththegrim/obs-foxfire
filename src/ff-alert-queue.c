#include "ff-alert-queue.h"
#include <string.h>

static const char *KIND_IDS[FF_ALERT_KIND_COUNT] = {
	[FF_ALERT_FOLLOW] = "follow", [FF_ALERT_SUB] = "sub",   [FF_ALERT_RESUB] = "resub",   [FF_ALERT_GIFT] = "gift",
	[FF_ALERT_BITS] = "bits",     [FF_ALERT_RAID] = "raid", [FF_ALERT_REDEEM] = "redeem",
};

const char *ff_alert_kind_id(enum ff_alert_kind k)
{
	if (k < 0 || k >= FF_ALERT_KIND_COUNT || !KIND_IDS[k])
		return "unknown";
	return KIND_IDS[k];
}

bool ff_alert_kind_parse(const char *id, enum ff_alert_kind *out)
{
	if (!id || !out)
		return false;
	for (int i = 0; i < FF_ALERT_KIND_COUNT; i++) {
		if (KIND_IDS[i] && !strcmp(KIND_IDS[i], id)) {
			*out = (enum ff_alert_kind)i;
			return true;
		}
	}
	return false;
}

void ff_alert_queue_init(struct ff_alert_queue *q)
{
	if (q)
		memset(q, 0, sizeof *q);
}

bool ff_alert_queue_push(struct ff_alert_queue *q, const struct ff_alert_event *e)
{
	if (!q || !e)
		return false;
	if (q->count >= FF_ALERT_QUEUE_MAX) {
		q->dropped++;
		return false;
	}
	q->items[(q->head + q->count) % FF_ALERT_QUEUE_MAX] = *e;
	q->count++;
	return true;
}

bool ff_alert_queue_pop(struct ff_alert_queue *q, struct ff_alert_event *out)
{
	if (!q || !out || q->count == 0)
		return false;
	*out = q->items[q->head];
	q->head = (q->head + 1) % FF_ALERT_QUEUE_MAX;
	q->count--;
	return true;
}

size_t ff_alert_queue_clear(struct ff_alert_queue *q)
{
	if (!q)
		return 0;
	size_t n = q->count;
	q->head = 0;
	q->count = 0;
	return n;
}
