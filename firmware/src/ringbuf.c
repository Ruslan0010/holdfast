/*
 * ringbuf.c — YOUR CODE GOES HERE.
 *
 * Read ringbuf.h first, then tests/test_ringbuf.c. The tests are the spec.
 * Run `make test`, watch it fail, make it pass one test at a time.
 *
 * Hints, in the order you will need them:
 *
 *   - The slot for a sequence number is  seq % rb->slot_count.
 *     That single line is the whole trick. Everything else is bookkeeping.
 *
 *   - To find a slot's memory:  rb->storage + (index * rb->slot_size)
 *     Pointer arithmetic on uint8_t* moves one byte per unit. That is why
 *     storage is uint8_t* and not void*.
 *
 *   - oldest_seq only moves when you are about to overwrite. Work out the
 *     condition on paper before you write it; off-by-one here is the classic
 *     bug and the tests will catch it.
 *
 *   - Check every pointer and every size before you touch memory. In firmware
 *     nobody is coming to catch your exception.
 *
 * Build with sanitizers while you work:  make test-asan
 */

#include "ringbuf.h"

#include <string.h>

int rb_init(rb_t *rb, void *storage, uint16_t *lengths,
            size_t slot_size, size_t slot_count)
{
    (void)rb; (void)storage; (void)lengths; (void)slot_size; (void)slot_count;
    return RB_ERR_ARG; /* TODO: implement me */
}

int rb_push(rb_t *rb, const void *data, size_t len, uint64_t *out_seq)
{
    (void)rb; (void)data; (void)len; (void)out_seq;
    return RB_ERR_ARG; /* TODO: implement me */
}

int rb_get(const rb_t *rb, uint64_t seq, void *out, size_t out_cap,
           uint16_t *out_len)
{
    (void)rb; (void)seq; (void)out; (void)out_cap; (void)out_len;
    return RB_ERR_ARG; /* TODO: implement me */
}

size_t rb_count(const rb_t *rb)
{
    (void)rb;
    return 0; /* TODO: implement me */
}

int rb_is_full(const rb_t *rb)
{
    (void)rb;
    return 0; /* TODO: implement me */
}
