/*
 * hal_mock.c — recording HAL for unit tests. See hal_mock.h.
 */

#include "hal_mock.h"

#include "hal/hal_adc.h"
#include "hal/hal_log.h"
#include "hal/hal_net.h"
#include "hal/hal_time.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t  sent[MOCK_MAX_SENT][MOCK_MAX_LEN];
static size_t   sent_len[MOCK_MAX_SENT];
static size_t   n_sent;
static uint64_t bytes_sent;
static int      fail_sends;

static uint8_t  rx_buf[MOCK_MAX_LEN];
static size_t   rx_len;

static uint64_t mono_now, wall_now;
static int      log_level = HAL_LOG_WARN;

void mock_reset(void)
{
    n_sent     = 0;
    bytes_sent = 0;
    fail_sends = 0;
    rx_len     = 0;
    memset(sent_len, 0, sizeof sent_len);
    log_level  = getenv("HAL_LOG_DEBUG") ? HAL_LOG_DEBUG : HAL_LOG_WARN;
}

size_t mock_sent_count(void)
{
    return n_sent;
}

const uint8_t *mock_sent(size_t i, size_t *len)
{
    if (i >= n_sent)
        return NULL;
    if (len)
        *len = sent_len[i];
    return sent[i];
}

uint64_t mock_bytes_sent(void)
{
    return bytes_sent;
}

void mock_fail_sends(int n)
{
    fail_sends = n;
}

void mock_queue_rx(const uint8_t *buf, size_t len)
{
    if (len > sizeof rx_buf)
        len = sizeof rx_buf;
    memcpy(rx_buf, buf, len);
    rx_len = len;
}

void mock_set_time(uint64_t mono_ns, uint64_t wall_ns)
{
    mono_now = mono_ns;
    wall_now = wall_ns;
}

/* --- HAL implementation ------------------------------------------------ */

int hal_net_init(const char *host, uint16_t port)
{
    (void)host;
    (void)port;
    return 0;
}

int hal_net_send(const void *buf, size_t len)
{
    if (fail_sends > 0) {
        fail_sends--;
        return -1;
    }
    if (n_sent >= MOCK_MAX_SENT || len > MOCK_MAX_LEN)
        return -1;
    memcpy(sent[n_sent], buf, len);
    sent_len[n_sent] = len;
    n_sent++;
    bytes_sent += len;
    return (int)len;
}

int hal_net_recv(void *buf, size_t cap, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (rx_len == 0)
        return 0;
    const size_t n = rx_len < cap ? rx_len : cap;
    memcpy(buf, rx_buf, n);
    rx_len = 0;
    return (int)n;
}

void hal_net_close(void)
{
}

uint64_t hal_time_mono_ns(void)
{
    return mono_now;
}

uint64_t hal_time_wall_ns(void)
{
    return wall_now;
}

uint8_t hal_time_clock_quality(void)
{
    return 2; /* PKT_CLOCK_LOCKED */
}

void hal_log(int level, const char *fmt, ...)
{
    if (level > log_level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("    [log] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void hal_log_set_level(int level)
{
    log_level = level;
}

int hal_adc_init(const char *source, int loop)
{
    (void)source;
    (void)loop;
    return 0;
}

int hal_adc_read(int32_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0;
}

void hal_adc_close(void)
{
}
