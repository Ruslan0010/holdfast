/*
 * test_packet.c — wire format: byte layout, round trips, and every way a
 * datagram can be malformed.
 */

#include "test.h"
#include "packet.h"
#include "crc32.h"
#include "wire.h"

#include <string.h>

static uint8_t buf[PKT_MAX_LEN];
static uint8_t payload[PKT_DATA_MAX_PAYLOAD];

static pkt_data_t sample_data(uint16_t n_samples)
{
    pkt_data_t d;
    memset(&d, 0, sizeof d);
    d.station_id       = 0x00C0FFEE;
    d.stream_id        = 7;
    d.clock_quality    = PKT_CLOCK_LOCKED;
    d.encoding         = PKT_ENC_RAW;
    d.seq              = 0x0102030405060708ull;
    d.t0_ns            = 1757400000123456789ull;
    d.sample_period_us = 10000;
    d.sample_count     = n_samples;
    d.payload_len      = (uint16_t)(n_samples * 4u);
    d.payload          = payload;
    for (size_t i = 0; i < sizeof payload; i++)
        payload[i] = (uint8_t)(i * 13 + 1);
    return d;
}

/* The layout in docs/PROTOCOL.md, byte for byte. If this test changes, the
 * document and the Python decoder change with it. */
static void test_data_byte_layout(void)
{
    pkt_data_t d = sample_data(2);
    size_t len = 0;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_OK);
    CHECK_EQ(len, 36 + 8 + 4);

    CHECK_EQ(buf[0], 'H');
    CHECK_EQ(buf[1], 'F');
    CHECK_EQ(buf[2], 1);            /* version */
    CHECK_EQ(buf[3], 1);            /* DATA */
    CHECK_EQ(rd_u32(buf + 4), 0x00C0FFEE);
    CHECK_EQ(rd_u16(buf + 8), 7);
    CHECK_EQ(buf[10], 0x02);        /* clock locked, encoding raw */
    CHECK_EQ(buf[11], 0);           /* reserved */
    CHECK_EQ(buf[12], 0x01);        /* seq, most significant byte first */
    CHECK_EQ(buf[19], 0x08);
    CHECK(rd_u64(buf + 20) == 1757400000123456789ull);
    CHECK_EQ(rd_u32(buf + 28), 10000);
    CHECK_EQ(rd_u16(buf + 32), 2);
    CHECK_EQ(rd_u16(buf + 34), 8);
    CHECK_MEM(buf + 36, payload, 8);
    CHECK_EQ(rd_u32(buf + 44), crc32_compute(buf, 44));
}

static void test_data_round_trip(void)
{
    pkt_data_t d = sample_data(54); /* fills the payload exactly */
    size_t len = 0;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_OK);
    CHECK_EQ(len, PKT_MAX_LEN);

    pkt_t p;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_OK);
    CHECK_EQ(p.type, PKT_DATA);
    CHECK_EQ(p.u.data.station_id, d.station_id);
    CHECK_EQ(p.u.data.stream_id, d.stream_id);
    CHECK_EQ(p.u.data.clock_quality, d.clock_quality);
    CHECK_EQ(p.u.data.encoding, d.encoding);
    CHECK(p.u.data.seq == d.seq);
    CHECK(p.u.data.t0_ns == d.t0_ns);
    CHECK_EQ(p.u.data.sample_period_us, d.sample_period_us);
    CHECK_EQ(p.u.data.sample_count, d.sample_count);
    CHECK_EQ(p.u.data.payload_len, d.payload_len);
    CHECK(p.u.data.payload == buf + PKT_DATA_HDR_LEN); /* no copy */
    CHECK_MEM(p.u.data.payload, payload, d.payload_len);
}

static void test_data_empty_payload_round_trip(void)
{
    pkt_data_t d = sample_data(0);
    size_t len = 0;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_OK);
    CHECK_EQ(len, PKT_DATA_HDR_LEN + PKT_CRC_LEN);
    pkt_t p;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_OK);
    CHECK_EQ(p.u.data.payload_len, 0);
    CHECK(p.u.data.payload == NULL);
}

static void test_heartbeat_round_trip(void)
{
    pkt_heartbeat_t h = { 42, 1000, 900, 86400, PKT_CLOCK_HOLDOVER };
    size_t len = 0;
    CHECK_EQ(pkt_encode_heartbeat(&h, buf, sizeof buf, &len), PKT_OK);
    CHECK_EQ(len, PKT_HEARTBEAT_LEN);
    CHECK_EQ(buf[3], 2);

    pkt_t p;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_OK);
    CHECK_EQ(p.type, PKT_HEARTBEAT);
    CHECK_EQ(p.u.heartbeat.station_id, 42);
    CHECK(p.u.heartbeat.next_seq == 1000);
    CHECK(p.u.heartbeat.oldest_seq == 900);
    CHECK_EQ(p.u.heartbeat.uptime_s, 86400);
    CHECK_EQ(p.u.heartbeat.clock_quality, PKT_CLOCK_HOLDOVER);
}

static void test_gaps_round_trip_full(void)
{
    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id = 42;
    g.count      = PKT_GAPS_MAX_RANGES;
    for (size_t i = 0; i < g.count; i++) {
        g.ranges[i].from = i * 100;
        g.ranges[i].to   = i * 100 + 5;
    }
    size_t len = 0;
    CHECK_EQ(pkt_encode_gaps(&g, buf, sizeof buf, &len), PKT_OK);
    CHECK_EQ(len, PKT_GAPS_MAX_LEN);
    CHECK(len <= PKT_MAX_LEN);

    pkt_t p;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_OK);
    CHECK_EQ(p.type, PKT_GAPS);
    CHECK_EQ(p.u.gaps.count, PKT_GAPS_MAX_RANGES);
    for (size_t i = 0; i < g.count; i++) {
        CHECK(p.u.gaps.ranges[i].from == i * 100);
        CHECK(p.u.gaps.ranges[i].to == i * 100 + 5);
    }
}

static void test_gaps_round_trip_empty(void)
{
    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id = 1;
    size_t len = 0;
    CHECK_EQ(pkt_encode_gaps(&g, buf, sizeof buf, &len), PKT_OK);
    CHECK_EQ(len, PKT_GAPS_HDR_LEN + PKT_CRC_LEN);
    pkt_t p;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_OK);
    CHECK_EQ(p.u.gaps.count, 0);
}

static void test_encoder_rejects_bad_input(void)
{
    pkt_data_t d = sample_data(2);
    size_t len = 0;
    uint8_t small[16];

    CHECK_EQ(pkt_encode_data(NULL, buf, sizeof buf, &len), PKT_ERR_ARG);
    CHECK_EQ(pkt_encode_data(&d, NULL, sizeof buf, &len), PKT_ERR_ARG);
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, NULL), PKT_ERR_ARG);
    CHECK_EQ(pkt_encode_data(&d, small, sizeof small, &len), PKT_ERR_SIZE);

    d.payload = NULL;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_ERR_ARG);

    d = sample_data(2);
    d.clock_quality = 3;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_ERR_FIELD);

    d = sample_data(2);
    d.encoding = 2;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_ERR_FIELD);

    d = sample_data(2);
    d.payload_len = 7; /* raw: must be 4 * sample_count */
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_ERR_FIELD);

    d = sample_data(55); /* 220 bytes, over the 216 limit */
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_ERR_FIELD);

    d = sample_data(0);
    d.encoding    = PKT_ENC_STEIM2;
    d.payload_len = 100; /* not a whole number of frames */
    d.sample_count = 10;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_ERR_FIELD);

    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.count = PKT_GAPS_MAX_RANGES + 1;
    CHECK_EQ(pkt_encode_gaps(&g, buf, sizeof buf, &len), PKT_ERR_FIELD);
    g.count = 1;
    g.ranges[0].from = 10;
    g.ranges[0].to   = 9;
    CHECK_EQ(pkt_encode_gaps(&g, buf, sizeof buf, &len), PKT_ERR_FIELD);

    pkt_heartbeat_t h = { 1, 5, 6, 0, PKT_CLOCK_FREE }; /* oldest > next */
    CHECK_EQ(pkt_encode_heartbeat(&h, buf, sizeof buf, &len), PKT_ERR_FIELD);
}

static void test_decoder_rejects_malformed(void)
{
    pkt_data_t d = sample_data(3);
    size_t len = 0;
    pkt_t p;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_OK);

    CHECK_EQ(pkt_decode(NULL, len, &p), PKT_ERR_ARG);
    CHECK_EQ(pkt_decode(buf, len, NULL), PKT_ERR_ARG);
    CHECK_EQ(pkt_decode(buf, 3, &p), PKT_ERR_SHORT);
    CHECK_EQ(pkt_decode(buf, 20, &p), PKT_ERR_SHORT);

    buf[0] = 'X';
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_MAGIC);
    buf[0] = 'H';

    buf[2] = 9;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_VERSION);
    buf[2] = PKT_VERSION;

    buf[3] = 200;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_TYPE);
    buf[3] = PKT_DATA;

    /* Trailing byte: a datagram is exactly one message. */
    CHECK_EQ(pkt_decode(buf, len + 1, &p), PKT_ERR_LEN);

    /* Declared payload longer than the datagram. */
    wr_u16(buf + 34, 100);
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_LEN);
    wr_u16(buf + 34, 217);
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_LEN);
    wr_u16(buf + 34, 12);

    /* Corrupt CRC. */
    buf[len - 1] ^= 0x01;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_CRC);
    buf[len - 1] ^= 0x01;

    /* Valid CRC but a field outside its domain: re-sign after tampering. */
    buf[10] = 0x03; /* clock quality 3 */
    wr_u32(buf + len - 4, crc32_compute(buf, len - 4));
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_FIELD);
    buf[10] = 0x02;
    wr_u16(buf + 32, 2); /* sample_count 2 with 12 payload bytes */
    wr_u32(buf + len - 4, crc32_compute(buf, len - 4));
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_FIELD);

    /* Heartbeat length must be exact. */
    pkt_heartbeat_t h = { 1, 2, 1, 0, 0 };
    CHECK_EQ(pkt_encode_heartbeat(&h, buf, sizeof buf, &len), PKT_OK);
    CHECK_EQ(pkt_decode(buf, len - 1, &p), PKT_ERR_SHORT);
    CHECK_EQ(pkt_decode(buf, len + 1, &p), PKT_ERR_LEN);

    /* Gaps with too many ranges, or an inverted range. */
    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.count = 1;
    g.ranges[0].to = 4;
    CHECK_EQ(pkt_encode_gaps(&g, buf, sizeof buf, &len), PKT_OK);
    buf[8] = 13;
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_LEN);
    buf[8] = 1;
    wr_u64(buf + 12, 9); /* from 9, to 4 */
    wr_u32(buf + len - 4, crc32_compute(buf, len - 4));
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_ERR_FIELD);
}

/* Every single-bit error in a datagram must be rejected. The CRC guarantees
 * it for the body; the checks before the CRC must not accidentally accept a
 * flipped header byte either. */
static void test_every_single_bit_flip_rejected(void)
{
    pkt_data_t d = sample_data(10);
    size_t len = 0;
    pkt_t p;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_OK);

    for (size_t byte = 0; byte < len; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[byte] ^= (uint8_t)(1u << bit);
            CHECK(pkt_decode(buf, len, &p) != PKT_OK);
            buf[byte] ^= (uint8_t)(1u << bit);
        }
    }
    CHECK_EQ(pkt_decode(buf, len, &p), PKT_OK); /* restored intact */
}

/* Every truncation of a valid datagram must be rejected without reading
 * past the end. Run under ASan this also proves the bounds checks. */
static void test_every_truncation_rejected(void)
{
    pkt_data_t d = sample_data(10);
    size_t len = 0;
    pkt_t p;
    CHECK_EQ(pkt_encode_data(&d, buf, sizeof buf, &len), PKT_OK);
    for (size_t n = 0; n < len; n++)
        CHECK(pkt_decode(buf, n, &p) != PKT_OK);
}

static void test_strerror_covers_all_codes(void)
{
    for (int rc = PKT_ERR_SIZE; rc <= PKT_OK; rc++)
        CHECK(strcmp(pkt_strerror(rc), "unknown error") != 0);
    CHECK_EQ(strcmp(pkt_strerror(-100), "unknown error"), 0);
}

int main(void)
{
    printf("packet\n");
    RUN(test_data_byte_layout);
    RUN(test_data_round_trip);
    RUN(test_data_empty_payload_round_trip);
    RUN(test_heartbeat_round_trip);
    RUN(test_gaps_round_trip_full);
    RUN(test_gaps_round_trip_empty);
    RUN(test_encoder_rejects_bad_input);
    RUN(test_decoder_rejects_malformed);
    RUN(test_every_single_bit_flip_rejected);
    RUN(test_every_truncation_rejected);
    RUN(test_strerror_covers_all_codes);
    return TEST_REPORT();
}
