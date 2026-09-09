/*
 * hal_net.h — datagram link to the server.
 *
 * The HAL is the line between code that is the product and code that is the
 * platform. Everything above it (ring buffer, packets, compression, the
 * station state machine) is plain C11 with no OS or hardware assumptions and
 * runs identically on a Linux host, under Zephyr, or bare metal. Everything
 * below it is replaced per target: firmware/hal/posix/ for the host, a
 * Zephyr socket implementation for the MCU, tests/mock/ for unit tests.
 *
 * One peer, fixed for the life of the process: a station talks to one server.
 */
#ifndef HAL_NET_H
#define HAL_NET_H

#include <stddef.h>
#include <stdint.h>

/* Resolve and remember the server. Returns 0 or a negative error. */
int hal_net_init(const char *host, uint16_t port);

/* Send one datagram. Returns bytes sent, or negative on error. A datagram
 * that the link drops is not an error here; the protocol deals with loss. */
int hal_net_send(const void *buf, size_t len);

/* Receive one datagram, waiting at most timeout_ms. Returns bytes received,
 * 0 on timeout, negative on error. Datagrams from anyone but the peer are
 * discarded below this line. */
int hal_net_recv(void *buf, size_t cap, uint32_t timeout_ms);

void hal_net_close(void);

#endif /* HAL_NET_H */
