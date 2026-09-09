/*
 * hal_mock.h — a HAL that records instead of transmitting.
 *
 * Every datagram the core sends is kept so a test can decode it and check
 * it byte for byte. Time is whatever the test says it is.
 */
#ifndef HAL_MOCK_H
#define HAL_MOCK_H

#include <stddef.h>
#include <stdint.h>

#define MOCK_MAX_SENT 256
#define MOCK_MAX_LEN  256

void           mock_reset(void);
size_t         mock_sent_count(void);
const uint8_t *mock_sent(size_t i, size_t *len);
uint64_t       mock_bytes_sent(void);

/* Make hal_net_send() fail with -1 for the next n calls. */
void mock_fail_sends(int n);

/* Queue a datagram for the next hal_net_recv(). */
void mock_queue_rx(const uint8_t *buf, size_t len);

void mock_set_time(uint64_t mono_ns, uint64_t wall_ns);

#endif /* HAL_MOCK_H */
