/*
 * ratelimit.c — token bucket. See ratelimit.h.
 *
 * Refill keeps an accumulator in byte-nanoseconds so that a rate that does
 * not divide a second evenly (say 300 bytes/s) still delivers exactly 300
 * bytes per second over time instead of rounding down on every call.
 */

#include "ratelimit.h"

#define NS_PER_S 1000000000ull

/* Longer than this between refills and the bucket is full anyway; clamping
 * keeps elapsed * rate inside 64 bits for any plausible rate. */
#define MAX_ELAPSED_NS (1000ull * NS_PER_S)

void rl_init(ratelimit_t *rl, uint64_t bytes_per_sec, uint64_t burst_bytes,
             uint64_t now_ns)
{
    if (rl == NULL)
        return;
    rl->rate_bps = bytes_per_sec;
    rl->burst    = burst_bytes;
    rl->tokens   = burst_bytes;
    rl->last_ns  = now_ns;
    rl->acc      = 0;
}

void rl_refill(ratelimit_t *rl, uint64_t now_ns)
{
    if (rl == NULL || rl->rate_bps == 0)
        return;

    /* A monotonic clock never goes backwards, but a mock or a badly ported
     * HAL might; treat it as no time passed rather than as a huge interval. */
    uint64_t elapsed = now_ns > rl->last_ns ? now_ns - rl->last_ns : 0;
    if (elapsed > MAX_ELAPSED_NS)
        elapsed = MAX_ELAPSED_NS;
    rl->last_ns = now_ns;

    rl->acc += elapsed * rl->rate_bps;
    const uint64_t add = rl->acc / NS_PER_S;
    rl->acc -= add * NS_PER_S;

    rl->tokens = rl->tokens + add > rl->burst ? rl->burst : rl->tokens + add;
    if (rl->tokens == rl->burst)
        rl->acc = 0; /* a full bucket does not bank fractional credit */
}

int rl_try_take(ratelimit_t *rl, size_t nbytes, uint64_t now_ns)
{
    if (rl == NULL)
        return 0;
    if (rl->rate_bps == 0)
        return 1;

    rl_refill(rl, now_ns);
    if ((uint64_t)nbytes > rl->tokens)
        return 0;
    rl->tokens -= nbytes;
    return 1;
}

uint64_t rl_available(const ratelimit_t *rl)
{
    return rl == NULL ? 0 : rl->tokens;
}
