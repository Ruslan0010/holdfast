/*
 * test_steim2.c — Steim2 round trips at every field width, capacity limits,
 * and the decoder's rejection of corrupt frames.
 */

#include "test.h"
#include "steim2.h"
#include "wire.h"

#include <string.h>

#define MAX_N 1024
static int32_t src[MAX_N];
static int32_t dst[MAX_N];
static uint8_t frames[4 * STEIM2_FRAME_LEN];

/* Deterministic pseudo-random generator so failures reproduce exactly. */
static uint32_t lcg_state = 12345;
static uint32_t lcg(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

static int round_trip(size_t n, size_t cap, size_t *consumed)
{
    size_t out_len = 0, n_out = 0;
    int rc = steim2_encode(src, n, frames, cap, &out_len, consumed);
    if (rc != STEIM2_OK)
        return rc;
    rc = steim2_decode(frames, out_len, *consumed, dst, MAX_N, &n_out);
    if (rc != STEIM2_OK)
        return rc;
    if (n_out != *consumed)
        return -100;
    return memcmp(src, dst, *consumed * sizeof src[0]) == 0 ? 0 : -101;
}

/* A flat signal: every difference is 0, seven per word. One frame holds
 * 13 data words = 91 samples; each later frame 15 words = 105. */
static void test_constant_signal_fills_frames(void)
{
    for (size_t i = 0; i < MAX_N; i++)
        src[i] = 1000;

    size_t consumed = 0;
    CHECK_EQ(round_trip(MAX_N, STEIM2_FRAME_LEN, &consumed), 0);
    CHECK_EQ(consumed, STEIM2_FIRST_FRAME_MAX_SAMPLES);

    CHECK_EQ(round_trip(MAX_N, 3 * STEIM2_FRAME_LEN, &consumed), 0);
    CHECK_EQ(consumed, STEIM2_FIRST_FRAME_MAX_SAMPLES + 2 * STEIM2_NEXT_FRAME_MAX_SAMPLES);
}

static void test_single_sample(void)
{
    src[0] = -123456;
    size_t consumed = 0;
    CHECK_EQ(round_trip(1, sizeof frames, &consumed), 0);
    CHECK_EQ(consumed, 1);
    CHECK_EQ(dst[0], -123456);
}

/* Differences exactly at the edge of each field width, both signs. A wrong
 * boundary in fits() or a wrong mask in pack_word() shows up here. */
static void test_every_width_boundary(void)
{
    const int32_t edges[] = { 7, -8, 15, -16, 31, -32, 127, -128,
                              511, -512, 16383, -16384, (1 << 29) - 1 };
    const size_t n_edges = sizeof edges / sizeof edges[0];

    size_t n = 0;
    src[n++] = 0;
    for (size_t e = 0; e < n_edges; e++) {
        src[n] = src[n - 1] + edges[e];
        n++;
        src[n] = src[n - 1] - edges[e]; /* back to where we were */
        n++;
    }
    /* The most negative 30-bit difference, -2^29, has no positive mirror
     * (2^29 does not fit), so climb back in two half steps. */
    src[n] = src[n - 1] - (1 << 29);
    n++;
    src[n] = src[n - 1] + (1 << 28);
    n++;
    src[n] = src[n - 1] + (1 << 28);
    n++;
    size_t consumed = 0;
    CHECK_EQ(round_trip(n, sizeof frames, &consumed), 0);
    CHECK_EQ(consumed, n);
}

/* Widths mixed at random so every packing and every transition between
 * packings is exercised across several frames. */
static void test_random_widths_multi_frame(void)
{
    lcg_state = 777;
    src[0] = 0;
    for (size_t i = 1; i < MAX_N; i++) {
        const unsigned bits = 1 + lcg() % 29;
        const int64_t  lim  = (int64_t)1 << (bits - 1);
        int64_t d = (int64_t)(lcg() % (uint32_t)(2 * lim)) - lim;
        int64_t v = (int64_t)src[i - 1] + d;
        /* Keep samples in a 28-bit range so every difference fits 30 bits. */
        if (v >= (1 << 27) || v < -(1 << 27))
            v = -(int64_t)src[i - 1] / 2;
        src[i] = (int32_t)v;
    }
    size_t total = 0;
    while (total < MAX_N) {
        size_t consumed = 0, out_len = 0, n_out = 0;
        CHECK_EQ(steim2_encode(src + total, MAX_N - total, frames, sizeof frames,
                               &out_len, &consumed), STEIM2_OK);
        CHECK(consumed >= 1);
        CHECK_EQ(steim2_decode(frames, out_len, consumed, dst, MAX_N, &n_out), STEIM2_OK);
        CHECK_EQ(n_out, consumed);
        CHECK_MEM(dst, src + total, consumed * sizeof src[0]);
        total += consumed;
    }
    CHECK_EQ(total, MAX_N);
}

/* A smooth 100 Hz signal like a real seismometer produces. 192 bytes of raw
 * int32 is 48 samples; Steim2 must do considerably better than that. */
static void test_compression_ratio_on_smooth_signal(void)
{
    for (size_t i = 0; i < MAX_N; i++) {
        /* Integer sine approximation, amplitude ~2000 counts, period 50. */
        const int32_t tri = (int32_t)(i % 50) < 25 ? (int32_t)(i % 50) : 50 - (int32_t)(i % 50);
        src[i] = tri * 80 - 1000 + (int32_t)(i % 3);
    }
    size_t consumed = 0;
    CHECK_EQ(round_trip(MAX_N, 3 * STEIM2_FRAME_LEN, &consumed), 0);
    CHECK(consumed >= 100); /* 2x better than raw at the very least */
}

static void test_out_of_range_difference_rejected(void)
{
    src[0] = 0;
    src[1] = 1 << 29; /* difference of 2^29 does not fit a 30-bit field */
    size_t out_len = 0, consumed = 0;
    CHECK_EQ(steim2_encode(src, 2, frames, sizeof frames, &out_len, &consumed),
             STEIM2_ERR_RANGE);
}

static void test_encoder_argument_checks(void)
{
    size_t out_len = 0, consumed = 0;
    CHECK_EQ(steim2_encode(NULL, 5, frames, sizeof frames, &out_len, &consumed), STEIM2_ERR_ARG);
    CHECK_EQ(steim2_encode(src, 5, NULL, sizeof frames, &out_len, &consumed), STEIM2_ERR_ARG);
    CHECK_EQ(steim2_encode(src, 5, frames, 63, &out_len, &consumed), STEIM2_ERR_SIZE);
    CHECK_EQ(steim2_encode(src, 0, frames, sizeof frames, &out_len, &consumed), STEIM2_OK);
    CHECK_EQ(out_len, 0);
    CHECK_EQ(consumed, 0);
}

/* Regression, found by the fuzzer: encoding zero samples produces zero
 * bytes, so decoding zero bytes with zero expected samples must succeed.
 * It used to return STEIM2_ERR_FORMAT, which made encode and decode
 * disagree at the empty case. */
static void test_empty_round_trip_is_symmetric(void)
{
    size_t out_len = 99, consumed = 99, n_out = 99;
    CHECK_EQ(steim2_encode(src, 0, frames, sizeof frames, &out_len, &consumed), STEIM2_OK);
    CHECK_EQ(out_len, 0);
    CHECK_EQ(consumed, 0);
    CHECK_EQ(steim2_decode(frames, out_len, consumed, dst, MAX_N, &n_out), STEIM2_OK);
    CHECK_EQ(n_out, 0);

    /* But asking for samples when there are no frames is still an error. */
    CHECK_EQ(steim2_decode(frames, 0, 1, dst, MAX_N, &n_out), STEIM2_ERR_SHORT);
}

static void test_decoder_detects_corruption(void)
{
    for (size_t i = 0; i < 50; i++)
        src[i] = (int32_t)(i * i);
    size_t out_len = 0, consumed = 0, n_out = 0;
    CHECK_EQ(steim2_encode(src, 50, frames, sizeof frames, &out_len, &consumed), STEIM2_OK);
    CHECK_EQ(consumed, 50);

    /* Wrong XN. */
    wr_u32(frames + 8, 0xDEADBEEF);
    CHECK_EQ(steim2_decode(frames, out_len, 50, dst, MAX_N, &n_out), STEIM2_ERR_INTEGRITY);
    wr_u32(frames + 8, (uint32_t)src[49]);
    CHECK_EQ(steim2_decode(frames, out_len, 50, dst, MAX_N, &n_out), STEIM2_OK);

    /* Ask for more samples than were encoded. */
    CHECK_EQ(steim2_decode(frames, out_len, 51, dst, MAX_N, &n_out), STEIM2_ERR_SHORT);

    /* Invalid nibble/dnib: nibble 11 with dnib 11 on word 3. */
    uint8_t saved[STEIM2_FRAME_LEN];
    memcpy(saved, frames, sizeof saved);
    uint32_t nib = rd_u32(frames);
    nib = (nib & ~(3u << 24)) | (3u << 24);
    wr_u32(frames, nib);
    wr_u32(frames + 12, 0xC0000000u);
    CHECK_EQ(steim2_decode(frames, out_len, 50, dst, MAX_N, &n_out), STEIM2_ERR_FORMAT);
    memcpy(frames, saved, sizeof saved);

    /* Not a whole number of frames; output too small. */
    CHECK_EQ(steim2_decode(frames, out_len - 1, 50, dst, MAX_N, &n_out), STEIM2_ERR_FORMAT);
    CHECK_EQ(steim2_decode(frames, out_len, 50, dst, 10, &n_out), STEIM2_ERR_SIZE);
}

/* Reconstruction that would overflow int32 must be an error, not UB. */
static void test_decoder_overflow_is_an_error(void)
{
    memset(frames, 0, sizeof frames);
    wr_u32(frames + 4, (uint32_t)INT32_MAX);      /* X0 */
    wr_u32(frames + 8, 0);                         /* XN, irrelevant */
    wr_u32(frames, 1u << 24);                      /* word 3: nibble 01 */
    wr_u32(frames + 12, 0x00010000u);              /* diffs 0, 1, 0, 0 */
    size_t n_out = 0;
    CHECK_EQ(steim2_decode(frames, STEIM2_FRAME_LEN, 2, dst, MAX_N, &n_out), STEIM2_ERR_FORMAT);
}

int main(void)
{
    printf("steim2\n");
    RUN(test_constant_signal_fills_frames);
    RUN(test_single_sample);
    RUN(test_every_width_boundary);
    RUN(test_random_widths_multi_frame);
    RUN(test_compression_ratio_on_smooth_signal);
    RUN(test_out_of_range_difference_rejected);
    RUN(test_encoder_argument_checks);
    RUN(test_empty_round_trip_is_symmetric);
    RUN(test_decoder_detects_corruption);
    RUN(test_decoder_overflow_is_an_error);
    return TEST_REPORT();
}
