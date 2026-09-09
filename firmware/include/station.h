/*
 * station.h — the field station's protocol state machine.
 *
 * This is the product. It turns a stream of samples into DATA packets,
 * retains them, pushes them to the server as the link allows, keeps the NAT
 * mapping alive with heartbeats, and answers backfill requests from the
 * retention buffer. It has no idea what a socket or a clock is: time comes
 * in as a parameter, bytes go out through hal_net_send(). That is what makes
 * it testable byte-for-byte with a mock HAL and portable to an MCU without
 * modification.
 *
 * Call pattern from the application loop:
 *
 *     station_init(&st, &cfg, slots, lens, n_slots, now);
 *     loop {
 *         station_feed(&st, samples, n, t_first, now);   // as they arrive
 *         if (hal_net_recv(...) > 0) station_on_rx(&st, buf, len, now);
 *         station_tick(&st, now);                        // often, ~10 Hz
 *     }
 *
 * Memory: everything is inside station_t and the caller-supplied slots. No
 * allocation after init.
 */
#ifndef STATION_H
#define STATION_H

#include <stddef.h>
#include <stdint.h>

#include "packet.h"
#include "ratelimit.h"
#include "ringbuf.h"

/* Samples buffered before they are packetised. Must exceed the largest
 * samples_per_packet by the largest single feed. */
#define STATION_MAX_PENDING 1024

/* Backfill sends per tick. Bounds the time one tick can take so the loop
 * keeps servicing the ADC. */
#define STATION_MAX_SENDS_PER_TICK 8

typedef struct {
    uint32_t station_id;
    uint16_t stream_id;
    uint32_t sample_period_us;     /* 10000 for 100 Hz */
    uint16_t samples_per_packet;   /* target; a packet never holds more */
    uint8_t  encoding;             /* PKT_ENC_RAW or PKT_ENC_STEIM2 */
    uint32_t heartbeat_interval_ms;
    uint32_t rate_limit_bps;       /* bytes per second, 0 = unlimited */
    uint32_t rate_burst_bytes;     /* >= PKT_MAX_LEN */
} station_cfg_t;

typedef struct {
    uint64_t samples_in;
    uint64_t packets_built;
    uint64_t live_sent;            /* DATA sent at build time */
    uint64_t live_deferred;        /* DATA held back by the rate limit */
    uint64_t retransmits;
    uint64_t heartbeats;
    uint64_t bytes_sent;           /* everything, including heartbeats */
    uint64_t gaps_received;        /* GAPS messages accepted */
    uint64_t gap_seqs_evicted;     /* requested sequence numbers already gone */
    uint64_t gap_seqs_future;      /* requested sequence numbers not yet made */
    uint64_t rx_bad;               /* datagrams that failed to decode */
    uint64_t send_errors;
} station_stats_t;

typedef struct {
    station_cfg_t   cfg;
    rb_t            rb;
    ratelimit_t     rl;

    int32_t         pending[STATION_MAX_PENDING];
    size_t          n_pending;
    uint64_t        pending_t0_ns; /* wall time of pending[0] */

    pkt_range_t     gaps[PKT_GAPS_MAX_RANGES];
    uint8_t         n_gaps;
    uint8_t         gap_idx;       /* range being served */
    uint64_t        gap_cursor;    /* next seq to resend within it */

    uint64_t        start_ns;
    uint64_t        last_heartbeat_ns;
    int             heartbeat_due; /* send one at the next tick regardless */
    uint8_t         clock_quality;

    uint8_t         txbuf[PKT_MAX_LEN];
    station_stats_t stats;
} station_t;

/* slots is n_slots * PKT_MAX_LEN bytes; lens holds n_slots entries.
 * Returns 0 or a negative error for a bad configuration. */
int station_init(station_t *st, const station_cfg_t *cfg, uint8_t *slots,
                 uint16_t *lens, size_t n_slots, uint64_t now_ns);

void station_set_clock_quality(station_t *st, uint8_t quality);

/* Accept samples; t_first_ns is the wall time of x[0]. Builds and sends
 * packets as soon as samples_per_packet are pending. Returns the number of
 * samples accepted, which is less than n only if the pending buffer is full. */
size_t station_feed(station_t *st, const int32_t *x, size_t n,
                    uint64_t t_first_ns, uint64_t now_ns);

/* Packetise whatever is pending, even a partial packet. For shutdown and
 * for latency bounds. Returns packets built. */
int station_flush(station_t *st, uint64_t now_ns);

/* Hand in a datagram from the server. */
void station_on_rx(station_t *st, const uint8_t *buf, size_t len,
                   uint64_t now_ns);

/* Heartbeats and backfill. Call frequently. */
void station_tick(station_t *st, uint64_t now_ns);

/* True if backfill requests are still being served. */
int station_backfill_pending(const station_t *st);

#endif /* STATION_H */
