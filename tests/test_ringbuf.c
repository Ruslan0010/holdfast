/*
 * test_ringbuf.c — the ring buffer specification, written as executable tests.
 *
 * Every test here describes a real situation a field station hits. The
 * comments say which one.
 */

#include "test.h"
#include "ringbuf.h"

#include <string.h>

#define SLOT_SIZE  32
#define SLOT_COUNT 4

/* Fixed, statically allocated backing store — exactly how you would do it on
 * a microcontroller with no heap. */
static uint8_t  storage[SLOT_SIZE * SLOT_COUNT];
static uint16_t lengths[SLOT_COUNT];
static rb_t     rb;

static void setup(void)
{
    memset(storage, 0, sizeof storage);
    memset(lengths, 0, sizeof lengths);
    memset(&rb, 0, sizeof rb);
}

/* Fill buf with a recognisable pattern so we can tell records apart. */
static void make_record(uint8_t *buf, size_t len, uint8_t tag)
{
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(tag + i);
}

/* ------------------------------------------------------------------ */

static void test_init_ok(void)
{
    setup();
    CHECK_EQ(rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT), RB_OK);
    CHECK_EQ(rb_count(&rb), 0);
    CHECK_EQ(rb.next_seq, 0);
    CHECK_EQ(rb.oldest_seq, 0);
    CHECK_EQ(rb_is_full(&rb), 0);
}

static void test_init_rejects_garbage(void)
{
    setup();
    CHECK_EQ(rb_init(NULL, storage, lengths, SLOT_SIZE, SLOT_COUNT), RB_ERR_ARG);
    CHECK_EQ(rb_init(&rb, NULL, lengths, SLOT_SIZE, SLOT_COUNT), RB_ERR_ARG);
    CHECK_EQ(rb_init(&rb, storage, NULL, SLOT_SIZE, SLOT_COUNT), RB_ERR_ARG);
    CHECK_EQ(rb_init(&rb, storage, lengths, 0, SLOT_COUNT), RB_ERR_ARG);
    CHECK_EQ(rb_init(&rb, storage, lengths, SLOT_SIZE, 0), RB_ERR_ARG);
}

/* Sequence numbers must start at 0 and increase by one, forever.
 * The server uses them to work out exactly what it is missing. */
static void test_push_assigns_sequential_seq(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t rec[8];
    uint64_t seq;

    for (uint64_t expected = 0; expected < 3; expected++) {
        make_record(rec, sizeof rec, (uint8_t)expected);
        CHECK_EQ(rb_push(&rb, rec, sizeof rec, &seq), RB_OK);
        CHECK_EQ(seq, expected);
    }
    CHECK_EQ(rb_count(&rb), 3);
}

/* What goes in comes out byte for byte. If this fails your memcpy or your
 * slot arithmetic is wrong. */
static void test_get_returns_exact_bytes(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t rec[20], out[SLOT_SIZE];
    uint64_t seq;
    uint16_t out_len = 0;

    make_record(rec, sizeof rec, 0xA0);
    CHECK_EQ(rb_push(&rb, rec, sizeof rec, &seq), RB_OK);

    CHECK_EQ(rb_get(&rb, seq, out, sizeof out, &out_len), RB_OK);
    CHECK_EQ(out_len, sizeof rec);
    CHECK_MEM(out, rec, sizeof rec);
}

/* Records of different lengths must not bleed into each other. A short
 * record written after a long one must report its own length. */
static void test_variable_lengths_do_not_bleed(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t long_rec[30], short_rec[4], out[SLOT_SIZE];
    uint64_t s_long, s_short;
    uint16_t out_len = 0;

    make_record(long_rec, sizeof long_rec, 0x10);
    make_record(short_rec, sizeof short_rec, 0xF0);

    CHECK_EQ(rb_push(&rb, long_rec, sizeof long_rec, &s_long), RB_OK);
    CHECK_EQ(rb_push(&rb, short_rec, sizeof short_rec, &s_short), RB_OK);

    CHECK_EQ(rb_get(&rb, s_short, out, sizeof out, &out_len), RB_OK);
    CHECK_EQ(out_len, sizeof short_rec);
    CHECK_MEM(out, short_rec, sizeof short_rec);

    CHECK_EQ(rb_get(&rb, s_long, out, sizeof out, &out_len), RB_OK);
    CHECK_EQ(out_len, sizeof long_rec);
    CHECK_MEM(out, long_rec, sizeof long_rec);
}

/* A record bigger than a slot is a programming error, not a runtime surprise.
 * Reject it, do not truncate, do not overflow. */
static void test_oversized_record_rejected(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t big[SLOT_SIZE + 1];
    uint64_t seq;
    memset(big, 0x5A, sizeof big);

    CHECK_EQ(rb_push(&rb, big, sizeof big, &seq), RB_ERR_SIZE);
    CHECK_EQ(rb_count(&rb), 0);  /* nothing was stored */
}

/* The link has been down for a week. Old data is gone. Say so clearly
 * instead of returning stale bytes. */
static void test_eviction_reports_evicted(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t rec[8], out[SLOT_SIZE];
    uint64_t seq;
    uint16_t out_len = 0;

    /* Push one more than capacity: seq 0 must fall out of the window. */
    for (int i = 0; i < SLOT_COUNT + 1; i++) {
        make_record(rec, sizeof rec, (uint8_t)i);
        CHECK_EQ(rb_push(&rb, rec, sizeof rec, &seq), RB_OK);
    }

    CHECK_EQ(rb_count(&rb), SLOT_COUNT);
    CHECK_EQ(rb.oldest_seq, 1);
    CHECK_EQ(rb_is_full(&rb), 1);
    CHECK_EQ(rb_get(&rb, 0, out, sizeof out, &out_len), RB_ERR_EVICTED);

    /* But seq 1 is the oldest survivor and must still be intact. */
    make_record(rec, sizeof rec, 1);
    CHECK_EQ(rb_get(&rb, 1, out, sizeof out, &out_len), RB_OK);
    CHECK_MEM(out, rec, sizeof rec);
}

/* Asking for a packet the station has not recorded yet is a different
 * failure from asking for one it has thrown away. The server reacts
 * differently to each, so do not collapse them into one error. */
static void test_future_seq_reports_future(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t rec[8], out[SLOT_SIZE];
    uint64_t seq;
    uint16_t out_len = 0;

    make_record(rec, sizeof rec, 1);
    rb_push(&rb, rec, sizeof rec, &seq);

    CHECK_EQ(rb_get(&rb, 1, out, sizeof out, &out_len), RB_ERR_FUTURE);
    CHECK_EQ(rb_get(&rb, 99999, out, sizeof out, &out_len), RB_ERR_FUTURE);
}

/* Empty buffer: every read is a read of the future. */
static void test_empty_buffer_reads_are_future(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t out[SLOT_SIZE];
    uint16_t out_len = 0;
    CHECK_EQ(rb_get(&rb, 0, out, sizeof out, &out_len), RB_ERR_FUTURE);
}

/* Caller's output buffer is too small. Refuse; do not write past the end.
 * This is the bug class that gets CVEs written about it. */
static void test_small_output_buffer_rejected(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t rec[20], out[4];
    uint64_t seq;
    uint16_t out_len = 0;

    make_record(rec, sizeof rec, 0x77);
    rb_push(&rb, rec, sizeof rec, &seq);

    CHECK_EQ(rb_get(&rb, seq, out, sizeof out, &out_len), RB_ERR_SIZE);
}

/* Three full laps of the buffer. Everything inside the window is correct,
 * everything outside it is honestly reported as gone. This is the test that
 * catches modulo mistakes. */
static void test_wraparound_three_laps(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    const int total = SLOT_COUNT * 3;
    uint8_t rec[16], out[SLOT_SIZE];
    uint64_t seq;
    uint16_t out_len = 0;

    for (int i = 0; i < total; i++) {
        make_record(rec, sizeof rec, (uint8_t)i);
        CHECK_EQ(rb_push(&rb, rec, sizeof rec, &seq), RB_OK);
        CHECK_EQ(seq, (uint64_t)i);
    }

    CHECK_EQ(rb_count(&rb), SLOT_COUNT);
    CHECK_EQ(rb.oldest_seq, (uint64_t)(total - SLOT_COUNT));

    /* Everything before the window is gone. */
    for (int i = 0; i < total - SLOT_COUNT; i++)
        CHECK_EQ(rb_get(&rb, (uint64_t)i, out, sizeof out, &out_len), RB_ERR_EVICTED);

    /* Everything inside the window is byte-perfect. */
    for (int i = total - SLOT_COUNT; i < total; i++) {
        make_record(rec, sizeof rec, (uint8_t)i);
        CHECK_EQ(rb_get(&rb, (uint64_t)i, out, sizeof out, &out_len), RB_OK);
        CHECK_EQ(out_len, sizeof rec);
        CHECK_MEM(out, rec, sizeof rec);
    }
}

/* The server lost the same retransmit twice and asks a third time.
 * Reading must not consume. */
static void test_get_is_repeatable(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t rec[12], out[SLOT_SIZE];
    uint64_t seq;
    uint16_t out_len = 0;

    make_record(rec, sizeof rec, 0x33);
    rb_push(&rb, rec, sizeof rec, &seq);

    for (int i = 0; i < 5; i++) {
        CHECK_EQ(rb_get(&rb, seq, out, sizeof out, &out_len), RB_OK);
        CHECK_MEM(out, rec, sizeof rec);
    }
    CHECK_EQ(rb_count(&rb), 1);
}

/* The station stamps each packet with rb_next_seq() before pushing it, so the
 * peek must agree with what rb_push() then assigns. */
static void test_next_seq_peek_matches_push(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t rec[4];
    uint64_t seq;
    for (int i = 0; i < SLOT_COUNT * 2; i++) {
        const uint64_t peek = rb_next_seq(&rb);
        make_record(rec, sizeof rec, (uint8_t)i);
        CHECK_EQ(rb_push(&rb, rec, sizeof rec, &seq), RB_OK);
        CHECK_EQ(seq, peek);
    }
    CHECK_EQ(rb_oldest_seq(&rb), SLOT_COUNT);
    CHECK_EQ(rb_next_seq(NULL), 0);
    CHECK_EQ(rb_oldest_seq(NULL), 0);
}

/* An uninitialised buffer must fail cleanly, not dereference garbage. */
static void test_uninitialised_buffer_rejected(void)
{
    setup();
    uint8_t out[SLOT_SIZE];
    uint64_t seq;
    uint16_t out_len = 0;
    CHECK_EQ(rb_push(&rb, out, 1, &seq), RB_ERR_ARG);
    CHECK_EQ(rb_get(&rb, 0, out, sizeof out, &out_len), RB_ERR_ARG);
    CHECK_EQ(rb_count(&rb), 0);
    CHECK_EQ(rb_is_full(&rb), 0);
    CHECK_EQ(rb_push(NULL, out, 1, &seq), RB_ERR_ARG);
    CHECK_EQ(rb_get(NULL, 0, out, sizeof out, &out_len), RB_ERR_ARG);
}

/* Zero-length record. Legal (a keepalive marker) and must round-trip. */
static void test_zero_length_record(void)
{
    setup();
    rb_init(&rb, storage, lengths, SLOT_SIZE, SLOT_COUNT);

    uint8_t out[SLOT_SIZE];
    uint64_t seq;
    uint16_t out_len = 99;

    CHECK_EQ(rb_push(&rb, out, 0, &seq), RB_OK);
    CHECK_EQ(rb_get(&rb, seq, out, sizeof out, &out_len), RB_OK);
    CHECK_EQ(out_len, 0);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("ringbuf\n");
    RUN(test_init_ok);
    RUN(test_init_rejects_garbage);
    RUN(test_push_assigns_sequential_seq);
    RUN(test_get_returns_exact_bytes);
    RUN(test_variable_lengths_do_not_bleed);
    RUN(test_oversized_record_rejected);
    RUN(test_eviction_reports_evicted);
    RUN(test_future_seq_reports_future);
    RUN(test_empty_buffer_reads_are_future);
    RUN(test_small_output_buffer_rejected);
    RUN(test_wraparound_three_laps);
    RUN(test_get_is_repeatable);
    RUN(test_next_seq_peek_matches_push);
    RUN(test_uninitialised_buffer_rejected);
    RUN(test_zero_length_record);
    return TEST_REPORT();
}
