/*
 * test_station.c — the station state machine against a recording HAL.
 *
 * Each test drives the core with samples, time and server datagrams, then
 * decodes what the core sent and checks it byte for byte.
 */

#include "test.h"
#include "station.h"
#include "steim2.h"
#include "wire.h"
#include "mock/hal_mock.h"

#include <string.h>

#define SLOTS 16
#define MS    1000000ull
#define S     1000000000ull
#define T0    1757400000000000000ull /* some wall time */

static uint8_t   slots[SLOTS * PKT_MAX_LEN];
static uint16_t  lens[SLOTS];
static station_t st;
static int32_t   sig[4096];

static station_cfg_t base_cfg(void)
{
    station_cfg_t c;
    memset(&c, 0, sizeof c);
    c.station_id            = 0x1234;
    c.stream_id             = 1;
    c.sample_period_us      = 10000; /* 100 Hz */
    c.samples_per_packet    = 50;
    c.encoding              = PKT_ENC_RAW;
    c.heartbeat_interval_ms = 10000;
    c.rate_limit_bps        = 0;
    c.rate_burst_bytes      = 0;
    return c;
}

static void setup(const station_cfg_t *cfg, size_t n_slots)
{
    mock_reset();
    CHECK_EQ(station_init(&st, cfg, slots, lens, n_slots, 0), 0);
    for (size_t i = 0; i < sizeof sig / sizeof sig[0]; i++)
        sig[i] = (int32_t)(i % 97) * 31 - 1500; /* small, varied differences */
}

/* Decode datagram i, whatever its type. */
static int decode_sent(size_t i, pkt_t *p)
{
    size_t len = 0;
    const uint8_t *b = mock_sent(i, &len);
    return b == NULL ? PKT_ERR_ARG : pkt_decode(b, len, p);
}

/* Decode datagram i as DATA or fail the calling test. */
#define DECODE_DATA(i, p)                                                     \
    do {                                                                      \
        size_t _len = 0;                                                      \
        const uint8_t *_dgram = mock_sent((i), &_len);                            \
        CHECK(_dgram != NULL);                                                    \
        CHECK_EQ(pkt_decode(_dgram, _len, &(p)), PKT_OK);                         \
        CHECK_EQ((p).type, PKT_DATA);                                         \
    } while (0)

static void test_init_rejects_bad_config(void)
{
    station_cfg_t c = base_cfg();
    mock_reset();
    CHECK_EQ(station_init(NULL, &c, slots, lens, SLOTS, 0), -1);
    CHECK_EQ(station_init(&st, NULL, slots, lens, SLOTS, 0), -1);
    CHECK_EQ(station_init(&st, &c, NULL, lens, SLOTS, 0), -1);
    CHECK_EQ(station_init(&st, &c, slots, lens, 0, 0), -1);
    c.samples_per_packet = 0;
    CHECK_EQ(station_init(&st, &c, slots, lens, SLOTS, 0), -1);
    c = base_cfg();
    c.encoding = 7;
    CHECK_EQ(station_init(&st, &c, slots, lens, SLOTS, 0), -1);
    c = base_cfg();
    c.rate_limit_bps   = 100;
    c.rate_burst_bytes = 10; /* smaller than one packet: nothing could ever go */
    CHECK_EQ(station_init(&st, &c, slots, lens, SLOTS, 0), -1);
}

/* 120 samples at 50 per packet: two packets out, 20 left pending, sequence
 * numbers 0 and 1, timestamps half a second apart. */
static void test_feed_builds_and_sends_packets(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, SLOTS);

    CHECK_EQ(station_feed(&st, sig, 120, T0, 0), 120);
    CHECK_EQ(mock_sent_count(), 2);
    CHECK_EQ(st.n_pending, 20);
    CHECK_EQ(st.stats.packets_built, 2);
    CHECK_EQ(st.stats.live_sent, 2);
    CHECK_EQ(rb_count(&st.rb), 2);

    pkt_t p;
    DECODE_DATA(0, p);
    CHECK(p.u.data.seq == 0);
    CHECK(p.u.data.t0_ns == T0);
    CHECK_EQ(p.u.data.sample_count, 50);
    CHECK_EQ(p.u.data.station_id, 0x1234);
    CHECK_EQ(p.u.data.encoding, PKT_ENC_RAW);
    for (size_t i = 0; i < 50; i++)
        CHECK_EQ((int32_t)rd_u32(p.u.data.payload + 4 * i), sig[i]);

    DECODE_DATA(1, p);
    CHECK(p.u.data.seq == 1);
    CHECK(p.u.data.t0_ns == T0 + 50 * 10 * MS);
    for (size_t i = 0; i < 50; i++)
        CHECK_EQ((int32_t)rd_u32(p.u.data.payload + 4 * i), sig[50 + i]);
}

static void test_flush_sends_partial_packet(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, SLOTS);
    station_feed(&st, sig, 120, T0, 0);
    CHECK_EQ(station_flush(&st, 0), 1);
    CHECK_EQ(st.n_pending, 0);
    pkt_t p;
    DECODE_DATA(2, p);
    CHECK_EQ(p.u.data.sample_count, 20);
    CHECK(p.u.data.t0_ns == T0 + 100 * 10 * MS);
    CHECK_EQ(station_flush(&st, 0), 0); /* nothing left */
}

/* Raw packets can hold at most 54 samples no matter what the target is. */
static void test_raw_packet_capped_at_payload(void)
{
    station_cfg_t c = base_cfg();
    c.samples_per_packet = 200;
    setup(&c, SLOTS);
    station_feed(&st, sig, 200, T0, 0);
    CHECK_EQ(mock_sent_count(), 1);
    pkt_t p;
    DECODE_DATA(0, p);
    CHECK_EQ(p.u.data.sample_count, 54);
    CHECK_EQ(st.n_pending, 146);
}

/* Steim2: feed a long run, decode everything that went out, and the
 * concatenation must equal the input. Fewer bytes than raw, too. */
static void test_steim2_stream_round_trips(void)
{
    station_cfg_t c = base_cfg();
    c.encoding           = PKT_ENC_STEIM2;
    c.samples_per_packet = 300;
    setup(&c, SLOTS);

    const size_t n = 2000;
    size_t fed = 0;
    while (fed < n)
        fed += station_feed(&st, sig + fed, n - fed > 128 ? 128 : n - fed,
                            T0 + fed * 10 * MS, 0);
    station_flush(&st, 0);

    static int32_t got[4096];
    size_t total = 0;
    uint64_t expect_t0 = T0;
    for (size_t i = 0; i < mock_sent_count(); i++) {
        pkt_t p;
        DECODE_DATA(i, p);
        CHECK(p.u.data.seq == i);
        CHECK(p.u.data.t0_ns == expect_t0);
        CHECK_EQ(p.u.data.encoding, PKT_ENC_STEIM2);
        size_t n_out = 0;
        CHECK_EQ(steim2_decode(p.u.data.payload, p.u.data.payload_len,
                               p.u.data.sample_count, got + total,
                               sizeof got / sizeof got[0] - total, &n_out), STEIM2_OK);
        total += n_out;
        expect_t0 += n_out * 10 * MS;
    }
    CHECK_EQ(total, n);
    CHECK_MEM(got, sig, n * sizeof sig[0]);
    CHECK(mock_bytes_sent() < n * 4); /* compressed beats raw */
    CHECK(mock_sent_count() <= n / 100);
}

static void test_heartbeat_on_first_tick_then_at_interval(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, SLOTS);
    station_feed(&st, sig, 100, T0, 0);        /* seq 0, 1 */
    CHECK_EQ(mock_sent_count(), 2);

    station_tick(&st, 1 * MS);                 /* first tick: announce */
    CHECK_EQ(mock_sent_count(), 3);
    pkt_t p;
    CHECK_EQ(decode_sent(2, &p), PKT_OK);
    CHECK_EQ(p.type, PKT_HEARTBEAT);
    CHECK_EQ(p.u.heartbeat.station_id, 0x1234);
    CHECK(p.u.heartbeat.next_seq == 2);
    CHECK(p.u.heartbeat.oldest_seq == 0);

    station_tick(&st, 9 * S);                  /* too soon */
    CHECK_EQ(mock_sent_count(), 3);
    station_tick(&st, 10 * S + 1 * MS);        /* interval elapsed */
    CHECK_EQ(mock_sent_count(), 4);
    CHECK_EQ(decode_sent(3, &p), PKT_OK);
    CHECK_EQ(p.type, PKT_HEARTBEAT);
    CHECK_EQ(p.u.heartbeat.uptime_s, 10);
    CHECK_EQ(st.stats.heartbeats, 2);
}

/* The server asks for [2..4] and [7..7]; the station resends exactly those
 * four, byte-identical to the originals. */
static void test_gaps_are_served_byte_identical(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, SLOTS);
    station_feed(&st, sig, 500, T0, 0);        /* seq 0..9 */
    station_tick(&st, 0);                      /* heartbeat, index 10 */
    CHECK_EQ(mock_sent_count(), 11);

    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id = 0x1234;
    g.count      = 2;
    g.ranges[0].from = 2; g.ranges[0].to = 4;
    g.ranges[1].from = 7; g.ranges[1].to = 7;
    uint8_t buf[PKT_MAX_LEN];
    size_t  len = 0;
    CHECK_EQ(pkt_encode_gaps(&g, buf, sizeof buf, &len), PKT_OK);
    station_on_rx(&st, buf, len, 0);
    CHECK_EQ(station_backfill_pending(&st), 1);
    CHECK_EQ(st.stats.gaps_received, 1);

    station_tick(&st, 1 * MS);
    CHECK_EQ(station_backfill_pending(&st), 0);
    CHECK_EQ(mock_sent_count(), 15);
    CHECK_EQ(st.stats.retransmits, 4);

    const uint64_t want[] = { 2, 3, 4, 7 };
    for (size_t i = 0; i < 4; i++) {
        size_t l1 = 0, l2 = 0;
        const uint8_t *orig  = mock_sent((size_t)want[i], &l1);
        const uint8_t *again = mock_sent(11 + i, &l2);
        CHECK_EQ(l1, l2);
        CHECK_MEM(orig, again, l1);
    }
}

/* Four slots, ten packets: 0..5 are gone. A request for everything must
 * yield 6..9, count the six evicted, and trigger an immediate heartbeat so
 * the server learns the new oldest_seq. */
static void test_evicted_requests_are_skipped_and_reported(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, 4);
    station_feed(&st, sig, 500, T0, 0);        /* seq 0..9 */
    station_tick(&st, 0);                      /* heartbeat */
    const size_t before = mock_sent_count();

    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id = 0x1234;
    g.count      = 1;
    g.ranges[0].from = 0; g.ranges[0].to = 9;
    uint8_t buf[PKT_MAX_LEN];
    size_t  len = 0;
    pkt_encode_gaps(&g, buf, sizeof buf, &len);
    station_on_rx(&st, buf, len, 0);
    station_tick(&st, 1 * S);

    CHECK_EQ(st.stats.retransmits, 4);
    CHECK_EQ(st.stats.gap_seqs_evicted, 6);
    CHECK_EQ(mock_sent_count(), before + 4 + 1); /* 4 resends + heartbeat */

    pkt_t p;
    CHECK_EQ(decode_sent(mock_sent_count() - 1, &p), PKT_OK);
    CHECK_EQ(p.type, PKT_HEARTBEAT);
    CHECK(p.u.heartbeat.oldest_seq == 6);
    CHECK(p.u.heartbeat.next_seq == 10);
}

static void test_future_requests_are_dropped(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, SLOTS);
    station_feed(&st, sig, 100, T0, 0);        /* seq 0, 1 */
    const size_t before = mock_sent_count();

    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id = 0x1234;
    g.count      = 2;
    g.ranges[0].from = 1;   g.ranges[0].to = 5;   /* 1 exists, 2..5 do not */
    g.ranges[1].from = 100; g.ranges[1].to = 200;
    uint8_t buf[PKT_MAX_LEN];
    size_t  len = 0;
    pkt_encode_gaps(&g, buf, sizeof buf, &len);
    station_on_rx(&st, buf, len, 0);
    station_tick(&st, 0);

    CHECK_EQ(st.stats.retransmits, 1);
    CHECK_EQ(st.stats.gap_seqs_future, 4 + 101);
    CHECK_EQ(station_backfill_pending(&st), 0);
    CHECK_EQ(mock_sent_count(), before + 1 + 1); /* one resend + first heartbeat */
}

/* 256 bytes/s with a 256-byte burst: one full packet per second. Five
 * packets built at once: the first goes, four are deferred, and backfill
 * later drains them at exactly one per second. */
static void test_rate_limit_defers_live_and_paces_backfill(void)
{
    station_cfg_t c = base_cfg();
    c.rate_limit_bps   = 256;
    c.rate_burst_bytes = 256;
    setup(&c, SLOTS);

    station_feed(&st, sig, 250, T0, 0);        /* five 236-byte packets */
    CHECK_EQ(st.stats.packets_built, 5);
    CHECK_EQ(st.stats.live_sent, 1);
    CHECK_EQ(st.stats.live_deferred, 4);

    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id = 0x1234;
    g.count      = 1;
    g.ranges[0].from = 1; g.ranges[0].to = 4;
    uint8_t buf[PKT_MAX_LEN];
    size_t  len = 0;
    pkt_encode_gaps(&g, buf, sizeof buf, &len);
    station_on_rx(&st, buf, len, 0);

    station_tick(&st, 100 * MS);               /* bucket still nearly empty */
    CHECK_EQ(st.stats.retransmits, 0);
    station_tick(&st, 1 * S);
    CHECK_EQ(st.stats.retransmits, 1);
    station_tick(&st, 1500 * MS);
    CHECK_EQ(st.stats.retransmits, 1);
    station_tick(&st, 2 * S);
    CHECK_EQ(st.stats.retransmits, 2);
    station_tick(&st, 10 * S);                 /* bucket holds one packet, not eight */
    CHECK_EQ(st.stats.retransmits, 3);
    station_tick(&st, 11 * S);
    CHECK_EQ(st.stats.retransmits, 4);
    CHECK_EQ(station_backfill_pending(&st), 0);
}

/* The heartbeat is the control channel; an empty bucket must not stop it. */
static void test_heartbeat_bypasses_rate_limit(void)
{
    station_cfg_t c = base_cfg();
    c.rate_limit_bps   = 256;
    c.rate_burst_bytes = 256;
    setup(&c, SLOTS);
    station_feed(&st, sig, 100, T0, 0);        /* bucket now empty */
    CHECK_EQ(st.stats.live_sent, 1);
    station_tick(&st, 1 * MS);
    CHECK_EQ(st.stats.heartbeats, 1);
}

/* Work per tick is bounded so the loop keeps servicing the ADC. */
static void test_backfill_work_per_tick_is_bounded(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, SLOTS);
    station_feed(&st, sig, 700, T0, 0);        /* seq 0..13 */

    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id = 0x1234;
    g.count      = 1;
    g.ranges[0].from = 0; g.ranges[0].to = 13;
    uint8_t buf[PKT_MAX_LEN];
    size_t  len = 0;
    pkt_encode_gaps(&g, buf, sizeof buf, &len);
    station_on_rx(&st, buf, len, 0);

    station_tick(&st, 0);
    CHECK_EQ(st.stats.retransmits, STATION_MAX_SENDS_PER_TICK);
    CHECK_EQ(station_backfill_pending(&st), 1);
    station_tick(&st, 1 * MS);
    CHECK_EQ(st.stats.retransmits, 14);
    CHECK_EQ(station_backfill_pending(&st), 0);
}

static void test_bad_and_foreign_datagrams_are_ignored(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, SLOTS);
    station_feed(&st, sig, 100, T0, 0);

    const uint8_t junk[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    station_on_rx(&st, junk, sizeof junk, 0);
    CHECK_EQ(st.stats.rx_bad, 1);

    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id = 0x9999;                     /* someone else's */
    g.count      = 1;
    g.ranges[0].to = 1;
    uint8_t buf[PKT_MAX_LEN];
    size_t  len = 0;
    pkt_encode_gaps(&g, buf, sizeof buf, &len);
    station_on_rx(&st, buf, len, 0);
    CHECK_EQ(st.stats.rx_bad, 2);
    CHECK_EQ(station_backfill_pending(&st), 0);

    /* A DATA packet addressed to us makes no sense either. */
    pkt_heartbeat_t h = { 0x1234, 1, 0, 0, 0 };
    pkt_encode_heartbeat(&h, buf, sizeof buf, &len);
    station_on_rx(&st, buf, len, 0);
    CHECK_EQ(st.stats.rx_bad, 3);
    station_on_rx(&st, NULL, 5, 0);            /* must not crash */
}

/* A failed send is counted and the packet stays retained for backfill. */
static void test_send_failure_is_counted_not_fatal(void)
{
    station_cfg_t c = base_cfg();
    setup(&c, SLOTS);
    mock_fail_sends(1);
    station_feed(&st, sig, 50, T0, 0);
    CHECK_EQ(st.stats.send_errors, 1);
    CHECK_EQ(st.stats.live_sent, 0);
    CHECK_EQ(rb_count(&st.rb), 1);
}

/* The pending buffer accepts what fits and reports the rest. */
static void test_feed_reports_partial_acceptance(void)
{
    station_cfg_t c = base_cfg();
    c.samples_per_packet = 400;
    setup(&c, SLOTS);
    CHECK_EQ(station_feed(&st, sig, 399, T0, 0), 399);
    CHECK_EQ(station_feed(&st, sig, 2000, T0, 0), STATION_MAX_PENDING - 399);
    CHECK_EQ(station_feed(&st, sig, 0, T0, 0), 0);
    CHECK_EQ(station_feed(&st, NULL, 5, T0, 0), 0);
}

int main(void)
{
    printf("station\n");
    RUN(test_init_rejects_bad_config);
    RUN(test_feed_builds_and_sends_packets);
    RUN(test_flush_sends_partial_packet);
    RUN(test_raw_packet_capped_at_payload);
    RUN(test_steim2_stream_round_trips);
    RUN(test_heartbeat_on_first_tick_then_at_interval);
    RUN(test_gaps_are_served_byte_identical);
    RUN(test_evicted_requests_are_skipped_and_reported);
    RUN(test_future_requests_are_dropped);
    RUN(test_rate_limit_defers_live_and_paces_backfill);
    RUN(test_heartbeat_bypasses_rate_limit);
    RUN(test_backfill_work_per_tick_is_bounded);
    RUN(test_bad_and_foreign_datagrams_are_ignored);
    RUN(test_send_failure_is_counted_not_fatal);
    RUN(test_feed_reports_partial_acceptance);
    return TEST_REPORT();
}
