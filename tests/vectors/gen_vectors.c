/*
 * gen_vectors.c — emit datagrams and Steim2 frames from the C encoders as
 * hex, one per line, for the Python implementation to decode.
 *
 * The two implementations were written independently from docs/PROTOCOL.md
 * and steim2.h. This is the file that proves they agree: `make vectors`
 * regenerates build/vectors.txt on every run and tests/python/test_cross.py
 * reads it, so a change to either side that breaks the other fails CI.
 */

#include "packet.h"
#include "steim2.h"

#include <stdio.h>
#include <string.h>

static void hex(const char *name, const uint8_t *b, size_t n)
{
    printf("%s ", name);
    for (size_t i = 0; i < n; i++)
        printf("%02x", b[i]);
    printf("\n");
}

int main(void)
{
    uint8_t buf[PKT_MAX_LEN];
    uint8_t payload[8];
    size_t  len = 0;

    for (size_t i = 0; i < sizeof payload; i++)
        payload[i] = (uint8_t)(i * 13 + 1);

    pkt_data_t d;
    memset(&d, 0, sizeof d);
    d.station_id       = 0x00C0FFEE;
    d.stream_id        = 7;
    d.clock_quality    = PKT_CLOCK_LOCKED;
    d.encoding         = PKT_ENC_RAW;
    d.seq              = 0x0102030405060708ull;
    d.t0_ns            = 1757400000123456789ull;
    d.sample_period_us = 10000;
    d.sample_count     = 2;
    d.payload_len      = 8;
    d.payload          = payload;
    pkt_encode_data(&d, buf, sizeof buf, &len);
    hex("DATA", buf, len);

    pkt_heartbeat_t h = { 42, 1000, 900, 86400, PKT_CLOCK_HOLDOVER };
    pkt_encode_heartbeat(&h, buf, sizeof buf, &len);
    hex("HEARTBEAT", buf, len);

    pkt_gaps_t g;
    memset(&g, 0, sizeof g);
    g.station_id     = 42;
    g.count          = 2;
    g.ranges[0].from = 10;
    g.ranges[0].to   = 20;
    g.ranges[1].from = 100;
    g.ranges[1].to   = 100;
    pkt_encode_gaps(&g, buf, sizeof buf, &len);
    hex("GAPS", buf, len);

    /* A signal with small differences and occasional large steps, so every
     * Steim2 packing appears. Regenerated in Python by the same formula. */
    int32_t x[400];
    for (int i = 0; i < 400; i++)
        x[i] = (i * i) % 1000 - 500 + ((i % 37 == 0) ? 40000 * (i % 3 - 1) : 0);

    uint8_t frames[3 * STEIM2_FRAME_LEN];
    size_t  out_len = 0, consumed = 0;
    steim2_encode(x, 400, frames, sizeof frames, &out_len, &consumed);
    printf("STEIM2_CONSUMED %zu\n", consumed);
    hex("STEIM2", frames, out_len);

    /* And a Steim2 DATA packet end to end. */
    d.encoding     = PKT_ENC_STEIM2;
    d.sample_count = (uint16_t)consumed;
    d.payload_len  = (uint16_t)out_len;
    d.payload      = frames;
    d.seq          = 99;
    pkt_encode_data(&d, buf, sizeof buf, &len);
    hex("DATA_STEIM2", buf, len);
    return 0;
}
