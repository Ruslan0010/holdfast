/*
 * steim2.c — Steim2 encoder and decoder. See steim2.h for the format.
 *
 * The encoder is greedy: for each word it tries the densest packing first
 * (seven 4-bit differences) and falls back to wider fields until the next
 * differences fit. This is what libmseed does and it is within a few percent
 * of optimal for real seismic data.
 *
 * No floating point, no heap, no recursion; all arithmetic on differences is
 * done in int64_t so that hostile input cannot cause signed overflow.
 */

#include "steim2.h"

#include "wire.h"

#include <string.h>

#define WORDS_PER_FRAME 16

/* A packing: k differences of `bits` bits each, marked by a nibble in W0 and,
 * for nibbles 10 and 11, a dnib in the top two bits of the word. */
typedef struct {
    uint8_t  k;
    uint8_t  bits;
    uint8_t  nibble;
    uint8_t  dnib;
} packing_t;

/* Densest first. */
static const packing_t packings[] = {
    { 7, 4,  3, 2 },
    { 6, 5,  3, 1 },
    { 5, 6,  3, 0 },
    { 4, 8,  1, 0 },
    { 3, 10, 2, 3 },
    { 2, 15, 2, 2 },
    { 1, 30, 2, 1 },
};
#define N_PACKINGS (sizeof packings / sizeof packings[0])

static int fits(int64_t d, unsigned bits)
{
    const int64_t lim = (int64_t)1 << (bits - 1);
    return d >= -lim && d < lim;
}

/* Difference of sample i from its predecessor; the first one is defined as 0
 * so a packet never depends on the previous packet. */
static int64_t diff_at(const int32_t *x, size_t i)
{
    return i == 0 ? 0 : (int64_t)x[i] - (int64_t)x[i - 1];
}

/* Pick the densest packing whose k differences, starting at i, all fit. */
static const packing_t *choose(const int32_t *x, size_t n, size_t i)
{
    for (size_t p = 0; p < N_PACKINGS; p++) {
        const packing_t *pk = &packings[p];
        if (i + pk->k > n)
            continue;
        int ok = 1;
        for (size_t j = 0; j < pk->k && ok; j++)
            ok = fits(diff_at(x, i + j), pk->bits);
        if (ok)
            return pk;
    }
    return NULL;
}

static uint32_t pack_word(const int32_t *x, size_t i, const packing_t *pk)
{
    const uint32_t mask = pk->bits == 32 ? 0xFFFFFFFFu : (1u << pk->bits) - 1u;
    uint32_t w = 0;
    for (size_t j = 0; j < pk->k; j++)
        w = (w << pk->bits) | ((uint32_t)diff_at(x, i + j) & mask);
    if (pk->nibble != 1)
        w |= (uint32_t)pk->dnib << 30;
    return w;
}

int steim2_encode(const int32_t *x, size_t n, uint8_t *out, size_t out_cap,
                  size_t *out_len, size_t *n_consumed)
{
    if (out == NULL || out_len == NULL || n_consumed == NULL)
        return STEIM2_ERR_ARG;
    if (x == NULL && n > 0)
        return STEIM2_ERR_ARG;
    if (out_cap < STEIM2_FRAME_LEN)
        return STEIM2_ERR_SIZE;

    *out_len    = 0;
    *n_consumed = 0;
    if (n == 0)
        return STEIM2_OK;

    const size_t max_frames = out_cap / STEIM2_FRAME_LEN;
    size_t i     = 0;
    size_t frame = 0;

    for (; frame < max_frames && i < n; frame++) {
        uint8_t *fp = out + frame * STEIM2_FRAME_LEN;
        memset(fp, 0, STEIM2_FRAME_LEN);

        uint32_t nibbles = 0;
        for (unsigned w = frame == 0 ? 3 : 1; w < WORDS_PER_FRAME && i < n; w++) {
            const packing_t *pk = choose(x, n, i);
            if (pk == NULL)
                return STEIM2_ERR_RANGE;
            wr_u32(fp + 4 * w, pack_word(x, i, pk));
            nibbles |= (uint32_t)pk->nibble << (2 * (15 - w));
            i += pk->k;
        }
        wr_u32(fp, nibbles);
    }

    wr_u32(out + 4, (uint32_t)x[0]);
    wr_u32(out + 8, (uint32_t)x[i - 1]);

    *out_len    = frame * STEIM2_FRAME_LEN;
    *n_consumed = i;
    return STEIM2_OK;
}

static int32_t sign_extend(uint32_t v, unsigned bits)
{
    const uint32_t sign = 1u << (bits - 1);
    v &= (sign << 1) - 1u;
    return (v & sign) ? (int32_t)(v - (sign << 1)) : (int32_t)v;
}

/* Work out how many differences a word holds and how wide they are. */
static int word_layout(unsigned nibble, uint32_t word, unsigned *k, unsigned *bits)
{
    const unsigned dnib = word >> 30;
    switch (nibble) {
    case 1:
        *k = 4; *bits = 8;
        return 1;
    case 2:
        if (dnib == 1) { *k = 1; *bits = 30; return 1; }
        if (dnib == 2) { *k = 2; *bits = 15; return 1; }
        if (dnib == 3) { *k = 3; *bits = 10; return 1; }
        return 0;
    case 3:
        if (dnib == 0) { *k = 5; *bits = 6; return 1; }
        if (dnib == 1) { *k = 6; *bits = 5; return 1; }
        if (dnib == 2) { *k = 7; *bits = 4; return 1; }
        return 0;
    default:
        return 0;
    }
}

int steim2_decode(const uint8_t *in, size_t in_len, size_t n_expected,
                  int32_t *out, size_t out_cap, size_t *n_out)
{
    if (in == NULL || out == NULL || n_out == NULL)
        return STEIM2_ERR_ARG;
    if (in_len == 0 || in_len % STEIM2_FRAME_LEN != 0)
        return STEIM2_ERR_FORMAT;
    if (n_expected > out_cap)
        return STEIM2_ERR_SIZE;

    *n_out = 0;
    if (n_expected == 0)
        return STEIM2_OK;

    const int32_t x0 = (int32_t)rd_u32(in + 4);
    const int32_t xn = (int32_t)rd_u32(in + 8);
    const size_t  frames = in_len / STEIM2_FRAME_LEN;

    size_t  count = 0;
    int64_t cur   = 0;

    for (size_t f = 0; f < frames && count < n_expected; f++) {
        const uint8_t *fp      = in + f * STEIM2_FRAME_LEN;
        const uint32_t nibbles = rd_u32(fp);

        for (unsigned w = f == 0 ? 3 : 1; w < WORDS_PER_FRAME && count < n_expected; w++) {
            const unsigned nibble = (nibbles >> (2 * (15 - w))) & 3u;
            if (nibble == 0)
                continue;

            const uint32_t word = rd_u32(fp + 4 * w);
            unsigned k, bits;
            if (!word_layout(nibble, word, &k, &bits))
                return STEIM2_ERR_FORMAT;

            for (unsigned j = 0; j < k && count < n_expected; j++) {
                const unsigned shift = bits * (k - 1 - j);
                const int32_t  d     = sign_extend(word >> shift, bits);

                if (count == 0)
                    cur = x0;
                else
                    cur += d;
                if (cur < INT32_MIN || cur > INT32_MAX)
                    return STEIM2_ERR_FORMAT;

                out[count++] = (int32_t)cur;
            }
        }
    }

    if (count < n_expected)
        return STEIM2_ERR_SHORT;
    if ((int32_t)cur != xn)
        return STEIM2_ERR_INTEGRITY;

    *n_out = count;
    return STEIM2_OK;
}
