/*
 * wire.h — big-endian field access for wire formats.
 *
 * Everything on the wire is big-endian (network byte order) and is written
 * and read byte by byte. A struct is never copied onto the wire: its layout
 * depends on the compiler's padding and the CPU's endianness, and a packet
 * built on a little-endian x86 host must decode identically on a big-endian
 * device or in a Python server. Byte-at-a-time access also never needs an
 * aligned pointer, which matters on Cortex-M0 where an unaligned 32-bit load
 * is a hard fault.
 */
#ifndef WIRE_H
#define WIRE_H

#include <stddef.h>
#include <stdint.h>

static inline void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static inline void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static inline void wr_u64(uint8_t *p, uint64_t v)
{
    wr_u32(p, (uint32_t)(v >> 32));
    wr_u32(p + 4, (uint32_t)(v & 0xFFFFFFFFu));
}

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

static inline uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t rd_u64(const uint8_t *p)
{
    return ((uint64_t)rd_u32(p) << 32) | (uint64_t)rd_u32(p + 4);
}

#endif /* WIRE_H */
