/*
 * hal_time.c (POSIX) — CLOCK_MONOTONIC for intervals, CLOCK_REALTIME for
 * timestamps, and the kernel's own opinion of whether the wall clock is
 * disciplined (adjtimex STA_UNSYNC) for clock quality.
 */

#define _DEFAULT_SOURCE

#include "hal/hal_time.h"

#include "packet.h"

#include <time.h>

#if defined(__linux__)
#include <sys/timex.h>
#endif

static uint64_t to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

uint64_t hal_time_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return to_ns(&ts);
}

uint64_t hal_time_wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return to_ns(&ts);
}

uint8_t hal_time_clock_quality(void)
{
#if defined(__linux__)
    struct timex tx;
    tx.modes = 0;
    const int state = adjtimex(&tx);
    if (state < 0)
        return PKT_CLOCK_FREE;
    if (state == TIME_ERROR || (tx.status & STA_UNSYNC))
        return PKT_CLOCK_FREE;
    return PKT_CLOCK_LOCKED;
#else
    return PKT_CLOCK_FREE;
#endif
}
