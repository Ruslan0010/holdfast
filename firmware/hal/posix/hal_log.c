/*
 * hal_log.c (POSIX) — stderr with a monotonic timestamp and a level tag.
 */

#include "hal/hal_log.h"

#include "hal/hal_time.h"

#include <stdarg.h>
#include <stdio.h>

static int current_level = HAL_LOG_INFO;

static const char *const level_tag[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

void hal_log(int level, const char *fmt, ...)
{
    if (level > current_level || level < HAL_LOG_ERROR || level > HAL_LOG_DEBUG)
        return;

    const uint64_t t  = hal_time_mono_ns();
    va_list        ap;
    fprintf(stderr, "%8llu.%03llu %s ", (unsigned long long)(t / 1000000000ull),
            (unsigned long long)(t / 1000000ull % 1000ull), level_tag[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void hal_log_set_level(int level)
{
    current_level = level;
}
