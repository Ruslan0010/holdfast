/*
 * ratelimit.h — token bucket in bytes per second.
 *
 * A field station must never exceed its link. A satellite modem at 2.4 kbit/s
 * that is handed 10 kbit/s does not queue politely; it drops, and it drops
 * the live data as readily as the backfill. So the station meters everything
 * it sends through one bucket. Live packets that cannot be sent stay in the
 * ring buffer and become backfill later, which is the right outcome: the
 * buffer is the queue.
 *
 * Integer arithmetic only. Time is nanoseconds from any monotonic source.
 */
#ifndef RATELIMIT_H
#define RATELIMIT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t rate_bps;  /* bytes per second; 0 means unlimited */
    uint64_t burst;     /* bucket capacity in bytes */
    uint64_t tokens;    /* bytes available right now */
    uint64_t last_ns;   /* when tokens was last brought up to date */
    uint64_t acc;       /* byte-nanoseconds not yet converted to tokens */
} ratelimit_t;

/* The bucket starts full. burst must be >= the largest single send. */
void rl_init(ratelimit_t *rl, uint64_t bytes_per_sec, uint64_t burst_bytes,
             uint64_t now_ns);

/* Bring tokens up to date. Called by rl_try_take; exposed for tests. */
void rl_refill(ratelimit_t *rl, uint64_t now_ns);

/* Take nbytes if available. Returns 1 and deducts them, or 0 and deducts
 * nothing. A request larger than burst can never succeed. */
int rl_try_take(ratelimit_t *rl, size_t nbytes, uint64_t now_ns);

/* Bytes available right now, for diagnostics. */
uint64_t rl_available(const ratelimit_t *rl);

#endif /* RATELIMIT_H */
