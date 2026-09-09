/*
 * test_crc32.c — CRC-32 against published vectors and a bit-serial reference.
 */

#include "test.h"
#include "crc32.h"

#include <string.h>

/* Slow, obviously-correct CRC-32: one bit at a time, no table. If the table
 * in crc32.c has a wrong entry, the two disagree. */
static uint32_t crc32_bitwise(const uint8_t *p, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}

/* The check value every CRC-32 implementation is compared against. */
static void test_known_vectors(void)
{
    CHECK_EQ(crc32_compute("123456789", 9), 0xCBF43926u);
    CHECK_EQ(crc32_compute("", 0), 0x00000000u);
    CHECK_EQ(crc32_compute("a", 1), 0xE8B7BE43u);
    CHECK_EQ(crc32_compute("abc", 3), 0x352441C2u);
}

/* The header and payload are checksummed in two calls on the wire path.
 * That must equal one call over the concatenation. */
static void test_incremental_equals_oneshot(void)
{
    const uint8_t msg[] = "The quick brown fox jumps over the lazy dog";
    uint32_t s = crc32_begin();
    s = crc32_feed(s, msg, 10);
    s = crc32_feed(s, msg + 10, sizeof msg - 10);
    CHECK_EQ(crc32_end(s), crc32_compute(msg, sizeof msg));
    CHECK_EQ(crc32_compute(msg, sizeof msg), crc32_bitwise(msg, sizeof msg));
}

/* Every byte value through the table, compared with the bit-serial form. */
static void test_table_matches_reference(void)
{
    uint8_t buf[512];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 7 + 3);
    for (size_t len = 0; len <= sizeof buf; len += 37)
        CHECK_EQ(crc32_compute(buf, len), crc32_bitwise(buf, len));
}

/* A single flipped bit anywhere must change the checksum. */
static void test_single_bit_flip_detected(void)
{
    uint8_t buf[64];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)i;
    const uint32_t good = crc32_compute(buf, sizeof buf);
    for (size_t byte = 0; byte < sizeof buf; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[byte] ^= (uint8_t)(1u << bit);
            CHECK(crc32_compute(buf, sizeof buf) != good);
            buf[byte] ^= (uint8_t)(1u << bit);
        }
    }
}

static void test_null_data_is_harmless(void)
{
    CHECK_EQ(crc32_compute(NULL, 0), 0x00000000u);
    CHECK_EQ(crc32_end(crc32_feed(crc32_begin(), NULL, 100)), 0x00000000u);
}

int main(void)
{
    printf("crc32\n");
    RUN(test_known_vectors);
    RUN(test_incremental_equals_oneshot);
    RUN(test_table_matches_reference);
    RUN(test_single_bit_flip_detected);
    RUN(test_null_data_is_harmless);
    return TEST_REPORT();
}
