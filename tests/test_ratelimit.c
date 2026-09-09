/*
 * test_ratelimit.c — token bucket accounting to the byte.
 */

#include "test.h"
#include "ratelimit.h"

#define MS 1000000ull
#define S  1000000000ull

static void test_starts_full_and_drains(void)
{
    ratelimit_t rl;
    rl_init(&rl, 1000, 500, 0);
    CHECK_EQ(rl_available(&rl), 500);
    CHECK_EQ(rl_try_take(&rl, 300, 0), 1);
    CHECK_EQ(rl_try_take(&rl, 200, 0), 1);
    CHECK_EQ(rl_try_take(&rl, 1, 0), 0);
    CHECK_EQ(rl_available(&rl), 0);
}

/* 1000 bytes/s: one byte per millisecond, no more, no less. */
static void test_refills_at_rate(void)
{
    ratelimit_t rl;
    rl_init(&rl, 1000, 500, 0);
    CHECK_EQ(rl_try_take(&rl, 500, 0), 1);
    CHECK_EQ(rl_try_take(&rl, 1, 999 * 1000), 0);   /* 0.999 ms: not yet */
    CHECK_EQ(rl_try_take(&rl, 1, 1 * MS), 1);       /* 1 ms: one byte */
    CHECK_EQ(rl_try_take(&rl, 1, 1 * MS), 0);
    CHECK_EQ(rl_try_take(&rl, 100, 101 * MS), 1);   /* 100 ms later: 100 */
    CHECK_EQ(rl_try_take(&rl, 1, 101 * MS), 0);
}

static void test_never_exceeds_burst(void)
{
    ratelimit_t rl;
    rl_init(&rl, 1000, 500, 0);
    rl_refill(&rl, 60 * S);
    CHECK_EQ(rl_available(&rl), 500);
    CHECK_EQ(rl_try_take(&rl, 501, 60 * S), 0);
    CHECK_EQ(rl_try_take(&rl, 500, 60 * S), 1);
}

/* 3 bytes/s does not divide a nanosecond-second evenly. Over 10 seconds it
 * must still deliver exactly 30 bytes, not 20 from rounding down. */
static void test_fractional_rate_does_not_drift(void)
{
    ratelimit_t rl;
    rl_init(&rl, 3, 1000, 0);
    CHECK_EQ(rl_try_take(&rl, 1000, 0), 1);
    uint64_t got = 0;
    for (uint64_t t = 333 * MS; t < 10 * S; t += 333 * MS)
        while (rl_try_take(&rl, 1, t))
            got++;
    CHECK(got < 30);                     /* 9.99 s: 29.97 bytes, not 30 */
    while (rl_try_take(&rl, 1, 10 * S))
        got++;
    CHECK_EQ(got, 30);
}

static void test_zero_rate_is_unlimited(void)
{
    ratelimit_t rl;
    rl_init(&rl, 0, 0, 0);
    for (int i = 0; i < 1000; i++)
        CHECK_EQ(rl_try_take(&rl, 100000, 0), 1);
}

static void test_time_going_backwards_is_harmless(void)
{
    ratelimit_t rl;
    rl_init(&rl, 1000, 100, 10 * S);
    CHECK_EQ(rl_try_take(&rl, 100, 10 * S), 1);
    CHECK_EQ(rl_try_take(&rl, 1, 5 * S), 0);        /* clock jumped back */
    CHECK_EQ(rl_try_take(&rl, 1, 5 * S + 1 * MS), 1); /* and moves on */
}

static void test_long_idle_does_not_overflow(void)
{
    ratelimit_t rl;
    rl_init(&rl, 10000000, 1000, 0);                  /* 10 MB/s */
    rl_try_take(&rl, 1000, 0);
    CHECK_EQ(rl_try_take(&rl, 1000, 365ull * 24 * 3600 * S), 1); /* a year */
    CHECK_EQ(rl_available(&rl), 0);
}

static void test_null_is_safe(void)
{
    rl_init(NULL, 1, 1, 0);
    rl_refill(NULL, 0);
    CHECK_EQ(rl_try_take(NULL, 1, 0), 0);
    CHECK_EQ(rl_available(NULL), 0);
}

int main(void)
{
    printf("ratelimit\n");
    RUN(test_starts_full_and_drains);
    RUN(test_refills_at_rate);
    RUN(test_never_exceeds_burst);
    RUN(test_fractional_rate_does_not_drift);
    RUN(test_zero_rate_is_unlimited);
    RUN(test_time_going_backwards_is_harmless);
    RUN(test_long_idle_does_not_overflow);
    RUN(test_null_is_safe);
    return TEST_REPORT();
}
