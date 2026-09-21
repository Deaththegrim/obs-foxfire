#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What arrived. Deliberately a plain struct with no pointers: it is copied into a queue that
   outlives whatever produced it, and a pointer into a websocket frame would not survive that. */
enum ff_alert_kind {
	FF_ALERT_FOLLOW = 0,
	FF_ALERT_SUB,
	FF_ALERT_RESUB,
	FF_ALERT_GIFT,
	FF_ALERT_BITS,
	FF_ALERT_RAID,
	FF_ALERT_REDEEM,
	FF_ALERT_KIND_COUNT,
};

struct ff_alert_event {
	enum ff_alert_kind kind;
	char name[192];    /* the sender, NOT yet sanitised -- that happens where it is drawn */
	char message[320]; /* their message, where the event carries one */
	int64_t amount;    /* bits, months, gift count, viewers raided with -- 0 when meaningless */
	int tier;          /* 1/2/3, 0 for Prime or not applicable */
};

/* The name a pack and the UI use for a kind ("follow", "sub", ...). Never NULL -- an unknown
   kind returns "unknown" rather than indexing off the end of a table. */
const char *ff_alert_kind_id(enum ff_alert_kind k);

/* Parses a kind id back. Returns false for anything unrecognised rather than guessing a default:
   an unrecognised kind silently becoming FOLLOW would show the wrong alert for a real event. */
bool ff_alert_kind_parse(const char *id, enum ff_alert_kind *out);

#define FF_ALERT_QUEUE_MAX 64

/* A fixed-capacity FIFO of pending alerts.
 *
 * Alerts arrive in bursts -- a raid lands dozens at once -- and each takes seconds to play, so
 * "show it now" is never the whole story. Everything here is deliberately pure and OBS-free so
 * the ordering and overflow behaviour can be tested without booting anything. */
struct ff_alert_queue {
	struct ff_alert_event items[FF_ALERT_QUEUE_MAX];
	size_t head, count;
	uint64_t dropped; /* events refused because the queue was full; surfaced, never silent */
};

void ff_alert_queue_init(struct ff_alert_queue *q);

/* Appends. Returns false and counts a drop when full.
 *
 * A full queue drops the NEWEST, not the oldest: the people already waiting asked first, and
 * discarding the front would mean a raid's alerts played in a shuffled order with the earliest
 * supporters missing. */
bool ff_alert_queue_push(struct ff_alert_queue *q, const struct ff_alert_event *e);

/* Pops the oldest into `out`. Returns false when empty. */
bool ff_alert_queue_pop(struct ff_alert_queue *q, struct ff_alert_event *out);

/* Throws everything away, returning how many were discarded -- for the "clear the queue" control
   a streamer needs when a raid has left them thirty alerts deep. */
size_t ff_alert_queue_clear(struct ff_alert_queue *q);
