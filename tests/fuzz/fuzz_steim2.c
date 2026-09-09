/*
 * fuzz_steim2.c — libFuzzer harness for the Steim2 decoder and encoder.
 *
 * The first input byte picks the expected sample count, the rest is treated
 * as frames. Property: whatever decodes must re-encode and decode back to
 * the same samples. The decoder must never overflow, over-read, or trust a
 * nibble it did not validate.
 */

#include "steim2.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MUST(cond) do { if (!(cond)) __builtin_trap(); } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;

    const size_t   n_expected = (size_t)data[0] * 2;
    const uint8_t *frames     = data + 1;
    const size_t   frames_len = (size - 1) / STEIM2_FRAME_LEN * STEIM2_FRAME_LEN;

    int32_t samples[512];
    size_t  n = 0;
    if (steim2_decode(frames, frames_len, n_expected, samples,
                      sizeof samples / sizeof samples[0], &n) != STEIM2_OK)
        return 0;
    MUST(n == n_expected);

    uint8_t re[8 * STEIM2_FRAME_LEN];
    size_t  re_len = 0, consumed = 0;
    if (steim2_encode(samples, n, re, sizeof re, &re_len, &consumed) != STEIM2_OK)
        return 0; /* a decoded stream can exceed 30-bit differences */

    int32_t back[512];
    size_t  n_back = 0;
    MUST(steim2_decode(re, re_len, consumed, back, sizeof back / sizeof back[0], &n_back) == STEIM2_OK);
    MUST(n_back == consumed);
    MUST(memcmp(back, samples, consumed * sizeof samples[0]) == 0);
    return 0;
}
