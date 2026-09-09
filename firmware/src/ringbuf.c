/*
 * ringbuf.c — sequence-indexed retransmit buffer. See ringbuf.h.
 *
 * The whole data structure is one line of arithmetic:
 *
 *     slot(seq) = seq % slot_count
 *
 * Because sequence numbers are assigned consecutively, consecutive records
 * land in consecutive slots and wrap around at the end of the storage. A
 * record with sequence number s is still in its slot exactly when no later
 * record has landed there, i.e. when s >= next_seq - slot_count. That bound
 * is oldest_seq, and it is the only piece of state besides next_seq.
 *
 * Neither counter ever wraps in practice: at 1000 records a second a 64-bit
 * sequence number lasts 584 million years.
 */

#include "ringbuf.h"

#include <string.h>

/* A buffer is usable only if rb_init() succeeded on it. Every entry point
 * re-checks this rather than trusting the caller, because on a device with
 * no memory protection a stray write into rb_t must not turn into a stray
 * write anywhere else. */
static int rb_valid(const rb_t *rb)
{
    return rb != NULL && rb->storage != NULL && rb->lengths != NULL &&
           rb->slot_size > 0 && rb->slot_count > 0;
}

static size_t slot_index(const rb_t *rb, uint64_t seq)
{
    return (size_t)(seq % rb->slot_count);
}

static uint8_t *slot_ptr(const rb_t *rb, size_t index)
{
    /* uint8_t arithmetic moves one byte per unit; that is why storage is
     * uint8_t* and not void*. */
    return rb->storage + index * rb->slot_size;
}

int rb_init(rb_t *rb, void *storage, uint16_t *lengths, size_t slot_size,
            size_t slot_count)
{
    if (rb == NULL || storage == NULL || lengths == NULL)
        return RB_ERR_ARG;
    if (slot_size == 0 || slot_size > UINT16_MAX || slot_count == 0)
        return RB_ERR_ARG;

    rb->storage    = storage;
    rb->lengths    = lengths;
    rb->slot_size  = slot_size;
    rb->slot_count = slot_count;
    rb->next_seq   = 0;
    rb->oldest_seq = 0;
    return RB_OK;
}

int rb_push(rb_t *rb, const void *data, size_t len, uint64_t *out_seq)
{
    if (!rb_valid(rb) || out_seq == NULL)
        return RB_ERR_ARG;
    if (data == NULL && len > 0)
        return RB_ERR_ARG;
    if (len > rb->slot_size)
        return RB_ERR_SIZE;

    const uint64_t seq   = rb->next_seq;
    const size_t   index = slot_index(rb, seq);

    if (len > 0)
        memcpy(slot_ptr(rb, index), data, len);
    rb->lengths[index] = (uint16_t)len;

    rb->next_seq = seq + 1;

    /* The window can hold slot_count records. If we now hold more than that,
     * the record we just overwrote was the oldest one; slide the window. */
    if (rb->next_seq - rb->oldest_seq > rb->slot_count)
        rb->oldest_seq = rb->next_seq - rb->slot_count;

    *out_seq = seq;
    return RB_OK;
}

int rb_get(const rb_t *rb, uint64_t seq, void *out, size_t out_cap,
           uint16_t *out_len)
{
    if (!rb_valid(rb) || out_len == NULL)
        return RB_ERR_ARG;

    /* Two distinct failures that the caller treats differently: "not yet"
     * means keep waiting, "gone" means stop asking. */
    if (seq >= rb->next_seq)
        return RB_ERR_FUTURE;
    if (seq < rb->oldest_seq)
        return RB_ERR_EVICTED;

    const size_t   index = slot_index(rb, seq);
    const uint16_t len   = rb->lengths[index];

    if (len > out_cap)
        return RB_ERR_SIZE;
    if (out == NULL && len > 0)
        return RB_ERR_ARG;

    if (len > 0)
        memcpy(out, slot_ptr(rb, index), len);
    *out_len = len;
    return RB_OK;
}

size_t rb_count(const rb_t *rb)
{
    if (!rb_valid(rb))
        return 0;
    return (size_t)(rb->next_seq - rb->oldest_seq);
}

int rb_is_full(const rb_t *rb)
{
    if (!rb_valid(rb))
        return 0;
    return rb_count(rb) == rb->slot_count;
}

uint64_t rb_next_seq(const rb_t *rb)
{
    return rb_valid(rb) ? rb->next_seq : 0;
}

uint64_t rb_oldest_seq(const rb_t *rb)
{
    return rb_valid(rb) ? rb->oldest_seq : 0;
}

int rb_resume(rb_t *rb, uint64_t start_seq)
{
    if (!rb_valid(rb) || rb->next_seq != rb->oldest_seq)
        return RB_ERR_ARG;
    rb->next_seq   = start_seq;
    rb->oldest_seq = start_seq;
    return RB_OK;
}
