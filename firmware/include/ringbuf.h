/*
 * ringbuf.h — sequence-indexed retransmit buffer for a field station.
 *
 * WHY THIS EXISTS
 * ---------------
 * A seismic station in the field loses its uplink constantly: the satellite
 * blinks, the modem re-registers, the power browns out. Samples keep arriving
 * from the ADC the whole time. So the station holds recent packets in a buffer
 * and, once the link returns, the server asks for the ones it never received.
 *
 * This is NOT a plain FIFO. A FIFO gives you the oldest item and forgets it.
 * Here the server may ask for *any* packet still retained, by sequence number,
 * possibly out of order, possibly more than once. So it is a fixed-capacity
 * window over an ever-growing sequence space:
 *
 *     seq:  ... 41  42 | 43  44  45  46 | (47 not written yet)
 *                      |<-- retained -->|
 *                    oldest           next-1
 *
 * Once the window slides past a sequence number, that data is gone forever,
 * and the buffer says so explicitly rather than returning stale bytes.
 *
 * DESIGN RULES
 * ------------
 *   1. No malloc. The caller owns the storage and passes it in.
 *   2. Fixed-size slots. Variable-length records mean fragmentation, and
 *      fragmentation on a device that must run for four years unattended is
 *      how you get a 3 a.m. phone call.
 *   3. Every function is O(1). No scanning, no shifting.
 *   4. No undefined behaviour on any input. Bad arguments return an error,
 *      they do not corrupt memory.
 *
 * THREADING
 * ---------
 * Not thread-safe by design. On an RTOS the producer (sampler thread) and the
 * consumer (uplink thread) must serialise access with a mutex or hand records
 * over through a queue. Keeping locking out of this module keeps it portable
 * and keeps the critical section visible at the call site.
 */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdint.h>

/* Return codes. Negative means failure, mirroring POSIX habit. */
#define RB_OK           0
#define RB_ERR_ARG     (-1) /* caller passed nonsense */
#define RB_ERR_EVICTED (-2) /* that sequence number has already been overwritten */
#define RB_ERR_FUTURE  (-3) /* that sequence number has not been written yet */
#define RB_ERR_SIZE    (-4) /* record too big for a slot, or output buffer too small */

typedef struct {
    uint8_t  *storage;    /* caller-owned memory, slot_size * slot_count bytes */
    uint16_t *lengths;    /* caller-owned, slot_count entries: bytes used per slot */
    size_t    slot_size;  /* bytes per slot */
    size_t    slot_count; /* number of slots (the retention window) */
    uint64_t  next_seq;   /* sequence number the next push will receive */
    uint64_t  oldest_seq; /* oldest sequence number still retained */
} rb_t;

/*
 * Initialise a buffer over caller-supplied memory.
 *
 * storage    must be at least slot_size * slot_count bytes
 * lengths    must hold at least slot_count uint16_t entries
 * slot_size  must be > 0 and <= UINT16_MAX
 * slot_count must be > 0
 *
 * On success the buffer is empty, next_seq == 0, oldest_seq == 0.
 * Returns RB_OK or RB_ERR_ARG.
 */
int rb_init(rb_t *rb, void *storage, uint16_t *lengths, size_t slot_size,
            size_t slot_count);

/*
 * Append a record. Assigns it the next sequence number.
 * When the buffer is full this overwrites the oldest record, which is the
 * correct behaviour for a field station: fresh data always matters more than
 * stale data you were never able to ship.
 *
 * On success writes the assigned sequence number to *out_seq and returns RB_OK.
 * Returns RB_ERR_SIZE if len > slot_size, RB_ERR_ARG on bad pointers.
 */
int rb_push(rb_t *rb, const void *data, size_t len, uint64_t *out_seq);

/*
 * Copy the record with the given sequence number into out.
 * Does not remove it — the server may ask again after a lost retransmit.
 *
 * On success writes the record length to *out_len and returns RB_OK.
 * Returns RB_ERR_EVICTED if seq has fallen out of the window,
 *         RB_ERR_FUTURE  if seq >= next_seq,
 *         RB_ERR_SIZE    if out_cap is smaller than the stored record.
 */
int rb_get(const rb_t *rb, uint64_t seq, void *out, size_t out_cap,
           uint16_t *out_len);

/* Number of records currently retained. */
size_t rb_count(const rb_t *rb);

/* True (1) if the next push will evict something. */
int rb_is_full(const rb_t *rb);

/* Sequence number the next push will be assigned. Lets a caller stamp a
 * record with its own sequence number before storing it. */
uint64_t rb_next_seq(const rb_t *rb);

/* Oldest sequence number still retained. Everything below it is gone. */
uint64_t rb_oldest_seq(const rb_t *rb);

#endif /* RINGBUF_H */
