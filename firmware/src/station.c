/*
 * station.c — the field station's protocol state machine. See station.h.
 *
 * Three rules shape this file:
 *
 *   1. The ring buffer is the queue. A packet is stored before it is sent,
 *      and if the rate limiter refuses to send it now, nothing else is done:
 *      the server will notice the hole from the next heartbeat and ask for
 *      it. One mechanism handles link loss, rate limiting and reboots alike.
 *
 *   2. Retransmits are byte-identical to the original. The stored bytes are
 *      the encoded datagram, so a resend is a copy and a send, with no
 *      re-encoding and no state that could differ from the first attempt.
 *
 *   3. Heartbeats bypass the rate limiter. They are 36 bytes at most every
 *      few seconds, they carry the retention window the server needs to
 *      decide what is still worth asking for, and the NAT mapping dies
 *      without them. Starving them to save 3 bytes/s would be a false economy.
 */

#include "station.h"

#include "hal/hal_log.h"
#include "hal/hal_net.h"
#include "steim2.h"
#include "wire.h"

#include <string.h>

/* Three Steim2 frames fit in a 216-byte payload; raw holds 54 samples. */
#define STEIM2_PAYLOAD_CAP (3u * STEIM2_FRAME_LEN)
#define RAW_MAX_SAMPLES    (PKT_DATA_MAX_PAYLOAD / 4u)

#define NS_PER_MS 1000000ull
#define NS_PER_US 1000ull

static int send_datagram(station_t *st, const uint8_t *buf, size_t len)
{
    const int rc = hal_net_send(buf, len);
    if (rc < 0) {
        st->stats.send_errors++;
        return rc;
    }
    st->stats.bytes_sent += len;
    return 0;
}

int station_init(station_t *st, const station_cfg_t *cfg, uint8_t *slots,
                 uint16_t *lens, size_t n_slots, uint64_t now_ns)
{
    if (st == NULL || cfg == NULL || slots == NULL || lens == NULL)
        return -1;
    if (cfg->sample_period_us == 0 || cfg->samples_per_packet == 0)
        return -1;
    if (cfg->samples_per_packet > STATION_MAX_PENDING / 2)
        return -1;
    if (cfg->encoding != PKT_ENC_RAW && cfg->encoding != PKT_ENC_STEIM2)
        return -1;
    if (cfg->rate_limit_bps != 0 && cfg->rate_burst_bytes < PKT_MAX_LEN)
        return -1;

    memset(st, 0, sizeof *st);
    st->cfg = *cfg;

    if (rb_init(&st->rb, slots, lens, PKT_MAX_LEN, n_slots) != RB_OK)
        return -1;
    rl_init(&st->rl, cfg->rate_limit_bps, cfg->rate_burst_bytes, now_ns);

    st->start_ns          = now_ns;
    st->last_heartbeat_ns = now_ns;
    st->heartbeat_due     = 1; /* announce ourselves at the first tick */
    st->clock_quality     = PKT_CLOCK_FREE;
    return 0;
}

void station_set_clock_quality(station_t *st, uint8_t quality)
{
    if (st != NULL && quality <= PKT_CLOCK_LOCKED)
        st->clock_quality = quality;
}

/* Encode the next packet's worth of pending samples into the payload area
 * of txbuf-to-be. Returns samples consumed, 0 on failure. */
static size_t encode_payload(station_t *st, uint8_t *payload, uint16_t *payload_len)
{
    size_t want = st->n_pending;
    if (want > st->cfg.samples_per_packet)
        want = st->cfg.samples_per_packet;

    if (st->cfg.encoding == PKT_ENC_RAW) {
        if (want > RAW_MAX_SAMPLES)
            want = RAW_MAX_SAMPLES;
        for (size_t i = 0; i < want; i++)
            wr_u32(payload + 4 * i, (uint32_t)st->pending[i]);
        *payload_len = (uint16_t)(want * 4);
        return want;
    }

    size_t out_len = 0, consumed = 0;
    const int rc = steim2_encode(st->pending, want, payload, STEIM2_PAYLOAD_CAP,
                                 &out_len, &consumed);
    if (rc != STEIM2_OK) {
        /* Only STEIM2_ERR_RANGE is reachable: a difference over 30 bits,
         * which no 24-bit ADC can produce. Drop the offending sample rather
         * than stall the stream. */
        hal_log(HAL_LOG_ERROR, "steim2 encode failed (%d), dropping a sample", rc);
        memmove(st->pending, st->pending + 1, (st->n_pending - 1) * sizeof st->pending[0]);
        st->n_pending--;
        st->pending_t0_ns += st->cfg.sample_period_us * NS_PER_US;
        return 0;
    }
    *payload_len = (uint16_t)out_len;
    return consumed;
}

/* Build one DATA packet from pending samples, store it, try to send it. */
static int emit_packet(station_t *st, uint64_t now_ns)
{
    uint8_t  payload[PKT_DATA_MAX_PAYLOAD];
    uint16_t payload_len = 0;

    const size_t consumed = encode_payload(st, payload, &payload_len);
    if (consumed == 0)
        return 0;

    pkt_data_t d;
    memset(&d, 0, sizeof d);
    d.station_id       = st->cfg.station_id;
    d.stream_id        = st->cfg.stream_id;
    d.clock_quality    = st->clock_quality;
    d.encoding         = st->cfg.encoding;
    d.seq              = rb_next_seq(&st->rb);
    d.t0_ns            = st->pending_t0_ns;
    d.sample_period_us = st->cfg.sample_period_us;
    d.sample_count     = (uint16_t)consumed;
    d.payload_len      = payload_len;
    d.payload          = payload;

    size_t len = 0;
    if (pkt_encode_data(&d, st->txbuf, sizeof st->txbuf, &len) != PKT_OK) {
        hal_log(HAL_LOG_ERROR, "packet encode failed; this is a bug");
        return 0;
    }

    uint64_t seq = 0;
    if (rb_push(&st->rb, st->txbuf, len, &seq) != RB_OK || seq != d.seq) {
        hal_log(HAL_LOG_ERROR, "ring buffer push failed; this is a bug");
        return 0;
    }

    /* Consume the samples. */
    st->n_pending -= consumed;
    memmove(st->pending, st->pending + consumed, st->n_pending * sizeof st->pending[0]);
    st->pending_t0_ns += (uint64_t)consumed * st->cfg.sample_period_us * NS_PER_US;
    st->stats.packets_built++;

    /* Rule 1: the buffer is the queue. */
    if (rl_try_take(&st->rl, len, now_ns)) {
        if (send_datagram(st, st->txbuf, len) == 0)
            st->stats.live_sent++;
    } else {
        st->stats.live_deferred++;
    }
    return 1;
}

size_t station_feed(station_t *st, const int32_t *x, size_t n,
                    uint64_t t_first_ns, uint64_t now_ns)
{
    if (st == NULL || (x == NULL && n > 0))
        return 0;

    size_t room = STATION_MAX_PENDING - st->n_pending;
    if (n > room)
        n = room;
    if (n == 0)
        return 0;

    if (st->n_pending == 0)
        st->pending_t0_ns = t_first_ns;
    memcpy(st->pending + st->n_pending, x, n * sizeof x[0]);
    st->n_pending += n;
    st->stats.samples_in += n;

    while (st->n_pending >= st->cfg.samples_per_packet)
        if (!emit_packet(st, now_ns))
            break;
    return n;
}

int station_flush(station_t *st, uint64_t now_ns)
{
    if (st == NULL)
        return 0;
    int built = 0;
    while (st->n_pending > 0) {
        if (!emit_packet(st, now_ns))
            break;
        built++;
    }
    return built;
}

static void send_heartbeat(station_t *st, uint64_t now_ns)
{
    pkt_heartbeat_t h;
    h.station_id    = st->cfg.station_id;
    h.next_seq      = rb_next_seq(&st->rb);
    h.oldest_seq    = rb_oldest_seq(&st->rb);
    h.uptime_s      = (uint32_t)((now_ns - st->start_ns) / 1000000000ull);
    h.clock_quality = st->clock_quality;

    uint8_t buf[PKT_HEARTBEAT_LEN];
    size_t  len = 0;
    if (pkt_encode_heartbeat(&h, buf, sizeof buf, &len) != PKT_OK)
        return;

    /* Rule 3: heartbeats bypass the rate limiter. */
    if (send_datagram(st, buf, len) == 0)
        st->stats.heartbeats++;
    st->last_heartbeat_ns = now_ns;
    st->heartbeat_due     = 0;
}

void station_on_rx(station_t *st, const uint8_t *buf, size_t len, uint64_t now_ns)
{
    (void)now_ns;
    if (st == NULL || buf == NULL)
        return;

    pkt_t p;
    const int rc = pkt_decode(buf, len, &p);
    if (rc != PKT_OK) {
        st->stats.rx_bad++;
        hal_log(HAL_LOG_WARN, "rx: %s (%zu bytes)", pkt_strerror(rc), len);
        return;
    }

    switch (p.type) {
    case PKT_GAPS:
        if (p.u.gaps.station_id != st->cfg.station_id) {
            st->stats.rx_bad++;
            return;
        }
        /* A new request replaces the old one: the server's view is newer. */
        memcpy(st->gaps, p.u.gaps.ranges, p.u.gaps.count * sizeof st->gaps[0]);
        st->n_gaps     = p.u.gaps.count;
        st->gap_idx    = 0;
        st->gap_cursor = st->n_gaps > 0 ? st->gaps[0].from : 0;
        st->stats.gaps_received++;
        hal_log(HAL_LOG_DEBUG, "rx: GAPS with %u ranges", p.u.gaps.count);
        return;
    case PKT_DATA:
    case PKT_HEARTBEAT:
    default:
        /* Only the server sends to us, and it only sends GAPS. */
        st->stats.rx_bad++;
        return;
    }
}

/* Advance to the next range once the current one is exhausted. */
static void next_range(station_t *st)
{
    st->gap_idx++;
    if (st->gap_idx < st->n_gaps)
        st->gap_cursor = st->gaps[st->gap_idx].from;
    else
        st->n_gaps = st->gap_idx = 0;
}

static void serve_backfill(station_t *st, uint64_t now_ns)
{
    int sends = 0;

    while (st->gap_idx < st->n_gaps && sends < STATION_MAX_SENDS_PER_TICK) {
        const pkt_range_t *r = &st->gaps[st->gap_idx];
        if (st->gap_cursor > r->to) {
            next_range(st);
            continue;
        }

        uint16_t len = 0;
        const int rc = rb_get(&st->rb, st->gap_cursor, st->txbuf, sizeof st->txbuf, &len);

        if (rc == RB_ERR_EVICTED) {
            /* Everything from here to the oldest retained packet is gone.
             * Skip it and tell the server so via the retention window. */
            const uint64_t oldest = rb_oldest_seq(&st->rb);
            const uint64_t lost_to = oldest > r->to + 1 ? r->to + 1 : oldest;
            st->stats.gap_seqs_evicted += lost_to - st->gap_cursor;
            st->gap_cursor    = lost_to;
            st->heartbeat_due = 1;
            continue;
        }
        if (rc == RB_ERR_FUTURE) {
            /* The server asked for packets that do not exist yet; its idea
             * of next_seq is stale. Drop the rest of this range. */
            st->stats.gap_seqs_future += r->to - st->gap_cursor + 1;
            next_range(st);
            continue;
        }
        if (rc != RB_OK) {
            hal_log(HAL_LOG_ERROR, "ring buffer read failed (%d); this is a bug", rc);
            next_range(st);
            continue;
        }

        if (!rl_try_take(&st->rl, len, now_ns))
            return; /* out of budget; resume at the next tick */

        /* Rule 2: byte-identical retransmit. */
        if (send_datagram(st, st->txbuf, len) == 0)
            st->stats.retransmits++;
        st->gap_cursor++;
        sends++;
    }
}

void station_tick(station_t *st, uint64_t now_ns)
{
    if (st == NULL)
        return;

    serve_backfill(st, now_ns);

    const uint64_t interval_ns = (uint64_t)st->cfg.heartbeat_interval_ms * NS_PER_MS;
    if (st->heartbeat_due || now_ns - st->last_heartbeat_ns >= interval_ns)
        send_heartbeat(st, now_ns);
}

int station_backfill_pending(const station_t *st)
{
    return st != NULL && st->gap_idx < st->n_gaps;
}
