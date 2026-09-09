/*
 * steim2.h — Steim2 compression for 32-bit seismic samples.
 *
 * Steim2 is the compression inside miniSEED, the archive format of every
 * seismic network. It encodes first differences between consecutive samples
 * into 64-byte frames, packing each 32-bit word with as many differences as
 * fit: seven 4-bit values for a quiet signal, one 30-bit value for a large
 * step. A quiet 100 Hz channel compresses about 5:1; a large earthquake still
 * compresses because most differences are small even when the samples are
 * not.
 *
 * Frame layout (16 big-endian 32-bit words):
 *
 *   W0        sixteen 2-bit nibbles, one per word, W0's own nibble is 00
 *   W1, W2    first frame only: X0 = first sample, XN = last sample
 *   W3..W15   difference words (W1..W15 in subsequent frames)
 *
 * Nibble  dnib (top 2 bits of the word)   contents
 *   00                                     no data
 *   01                                     four 8-bit differences
 *   10      01                             one 30-bit difference
 *   10      10                             two 15-bit differences
 *   10      11                             three 10-bit differences
 *   11      00                             five 6-bit differences
 *   11      01                             six 5-bit differences
 *   11      10                             seven 4-bit differences
 *
 * The first difference of a packet is stored as 0. Each packet is therefore
 * self-contained: it decodes without the previous packet, which on a lossy
 * link may never arrive. XN is a built-in integrity check: after reconstruction
 * the last sample must equal it, or the decoder reports an error.
 *
 * Constraint: every difference must fit in 30 bits. A 24-bit ADC can never
 * violate that. Out-of-range input is reported, not truncated.
 */
#ifndef STEIM2_H
#define STEIM2_H

#include <stddef.h>
#include <stdint.h>

#define STEIM2_FRAME_LEN 64u

/* Maximum samples one frame can hold (seven 4-bit differences per word). */
#define STEIM2_FIRST_FRAME_MAX_SAMPLES (13u * 7u)
#define STEIM2_NEXT_FRAME_MAX_SAMPLES  (15u * 7u)

#define STEIM2_OK             0
#define STEIM2_ERR_ARG       (-1) /* NULL pointer or zero output capacity */
#define STEIM2_ERR_SIZE      (-2) /* output buffer cannot hold the result */
#define STEIM2_ERR_RANGE     (-3) /* a difference does not fit in 30 bits */
#define STEIM2_ERR_FORMAT    (-4) /* input is not valid Steim2 */
#define STEIM2_ERR_SHORT     (-5) /* input holds fewer samples than expected */
#define STEIM2_ERR_INTEGRITY (-6) /* last sample does not match XN */

/*
 * Encode as many samples from x as fit in whole frames of out.
 *
 * out_cap is rounded down to a multiple of 64; at least one frame is needed.
 * On success *out_len is the number of bytes written (a multiple of 64) and
 * *n_consumed the number of samples encoded, which is at least 1 when n >= 1
 * and may be less than n when the frames fill up. The caller sends the
 * remainder in the next packet.
 */
int steim2_encode(const int32_t *x, size_t n, uint8_t *out, size_t out_cap,
                  size_t *out_len, size_t *n_consumed);

/*
 * Decode n_expected samples from in (a whole number of frames) into out.
 *
 * Returns STEIM2_ERR_SHORT if the frames hold fewer samples than expected,
 * STEIM2_ERR_INTEGRITY if the reconstructed last sample differs from XN,
 * STEIM2_ERR_FORMAT for an invalid nibble/dnib combination or arithmetic
 * overflow. On success *n_out == n_expected.
 */
int steim2_decode(const uint8_t *in, size_t in_len, size_t n_expected,
                  int32_t *out, size_t out_cap, size_t *n_out);

#endif /* STEIM2_H */
