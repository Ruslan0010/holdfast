/*
 * fuzz_packet.c — libFuzzer harness for the datagram decoder.
 *
 * Property: anything the decoder accepts can be re-encoded and decoded
 * again to the same fields, and a Steim2 payload it accepts decodes without
 * undefined behaviour. Anything it rejects must be rejected without reading
 * past the end of the input, which ASan enforces.
 *
 *   make fuzz         libFuzzer with clang, bounded time, seed corpus
 *   make fuzz-corpus  replay the seed corpus with any compiler
 */

#include "packet.h"
#include "steim2.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MUST(cond) do { if (!(cond)) __builtin_trap(); } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    pkt_t p;
    if (pkt_decode(data, size, &p) != PKT_OK)
        return 0;

    uint8_t out[PKT_MAX_LEN];
    size_t  len = 0;
    pkt_t   q;

    switch (p.type) {
    case PKT_DATA: {
        MUST(pkt_encode_data(&p.u.data, out, sizeof out, &len) == PKT_OK);
        MUST(pkt_decode(out, len, &q) == PKT_OK && q.type == PKT_DATA);
        MUST(q.u.data.station_id == p.u.data.station_id);
        MUST(q.u.data.stream_id == p.u.data.stream_id);
        MUST(q.u.data.clock_quality == p.u.data.clock_quality);
        MUST(q.u.data.encoding == p.u.data.encoding);
        MUST(q.u.data.seq == p.u.data.seq);
        MUST(q.u.data.t0_ns == p.u.data.t0_ns);
        MUST(q.u.data.sample_period_us == p.u.data.sample_period_us);
        MUST(q.u.data.sample_count == p.u.data.sample_count);
        MUST(q.u.data.payload_len == p.u.data.payload_len);
        MUST(p.u.data.payload_len == 0 ||
             memcmp(q.u.data.payload, p.u.data.payload, p.u.data.payload_len) == 0);

        if (p.u.data.encoding == PKT_ENC_STEIM2) {
            int32_t samples[1024];
            size_t  n = 0;
            (void)steim2_decode(p.u.data.payload, p.u.data.payload_len,
                                p.u.data.sample_count, samples,
                                sizeof samples / sizeof samples[0], &n);
        }
        break;
    }
    case PKT_HEARTBEAT:
        MUST(pkt_encode_heartbeat(&p.u.heartbeat, out, sizeof out, &len) == PKT_OK);
        MUST(pkt_decode(out, len, &q) == PKT_OK && q.type == PKT_HEARTBEAT);
        MUST(memcmp(&q.u.heartbeat, &p.u.heartbeat, sizeof p.u.heartbeat) == 0);
        break;
    case PKT_GAPS:
        MUST(pkt_encode_gaps(&p.u.gaps, out, sizeof out, &len) == PKT_OK);
        MUST(pkt_decode(out, len, &q) == PKT_OK && q.type == PKT_GAPS);
        MUST(q.u.gaps.station_id == p.u.gaps.station_id);
        MUST(q.u.gaps.count == p.u.gaps.count);
        MUST(memcmp(q.u.gaps.ranges, p.u.gaps.ranges,
                    p.u.gaps.count * sizeof p.u.gaps.ranges[0]) == 0);
        break;
    default:
        __builtin_trap();
    }
    return 0;
}
