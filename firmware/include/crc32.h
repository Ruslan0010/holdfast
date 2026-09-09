/*
 * crc32.h — CRC-32 (IEEE 802.3), the same checksum as zlib, Ethernet, PNG.
 *
 * Reflected algorithm, polynomial 0xEDB88320, initial value 0xFFFFFFFF, final
 * complement. Choosing the most common variant is deliberate: the server can
 * verify packets with zlib.crc32() and never share a line of code with the
 * firmware, so a bug in one side cannot hide a bug in the other.
 *
 * Table-driven: 256 entries, 1 KiB, held in flash as a const array. Byte at
 * a time, roughly 8 cycles per byte on a Cortex-M4, which for a 256-byte
 * packet at 100 Hz is nothing.
 */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/* One-shot: CRC-32 of a buffer. */
uint32_t crc32_compute(const void *data, size_t len);

/* Incremental interface for data that arrives in pieces (header, then
 * payload). Usage:
 *
 *     uint32_t s = crc32_begin();
 *     s = crc32_feed(s, hdr, hdr_len);
 *     s = crc32_feed(s, payload, payload_len);
 *     uint32_t crc = crc32_end(s);
 */
uint32_t crc32_begin(void);
uint32_t crc32_feed(uint32_t state, const void *data, size_t len);
uint32_t crc32_end(uint32_t state);

#endif /* CRC32_H */
