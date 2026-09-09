/* Minimal test harness. ~40 lines, no dependencies.
 * Read it once and you know exactly how your tests work.
 * At Milestone 3 you will swap this for Unity or CMocka. */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;
static const char *current_test = "";

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            tests_failed++;                                                 \
            return;                                                         \
        }                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                      \
    do {                                                                    \
        long long _a = (long long)(a), _b = (long long)(b);                 \
        if (_a != _b) {                                                     \
            printf("  FAIL %s:%d  %s == %s  (got %lld, want %lld)\n",       \
                   __FILE__, __LINE__, #a, #b, _a, _b);                     \
            tests_failed++;                                                 \
            return;                                                         \
        }                                                                   \
    } while (0)

#define CHECK_MEM(a, b, n)                                                  \
    do {                                                                    \
        if (memcmp((a), (b), (n)) != 0) {                                   \
            printf("  FAIL %s:%d  memory differs (%s vs %s)\n",             \
                   __FILE__, __LINE__, #a, #b);                             \
            tests_failed++;                                                 \
            return;                                                         \
        }                                                                   \
    } while (0)

#define RUN(fn)                                                             \
    do {                                                                    \
        current_test = #fn;                                                 \
        tests_run++;                                                        \
        int _before = tests_failed;                                         \
        fn();                                                               \
        if (_before == tests_failed) printf("  ok   %s\n", #fn);            \
    } while (0)

#define TEST_REPORT()                                                       \
    (printf("\n%d run, %d failed\n", tests_run, tests_failed),              \
     tests_failed == 0 ? 0 : 1)

#endif /* TEST_H */
