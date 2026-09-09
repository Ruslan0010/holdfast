/*
 * hal_net.c (POSIX) — UDP to one server over a connected socket.
 *
 * connect() on a UDP socket does two useful things: send() needs no address,
 * and the kernel discards datagrams from anyone but the peer. It also means
 * an ICMP "port unreachable" surfaces as ECONNREFUSED on the next call,
 * which the station treats like any other lost datagram: the packet is
 * still in the ring buffer and the server will ask for it.
 */

#include "hal/hal_net.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int fd = -1;

int hal_net_init(const char *host, uint16_t port)
{
    if (host == NULL)
        return -1;

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL)
        return -1;

    int rc = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            rc = 0;
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return rc;
}

int hal_net_send(const void *buf, size_t len)
{
    if (fd < 0)
        return -1;
    const ssize_t n = send(fd, buf, len, 0);
    return n < 0 ? -1 : (int)n;
}

int hal_net_recv(void *buf, size_t cap, uint32_t timeout_ms)
{
    if (fd < 0)
        return -1;

    struct pollfd p = { fd, POLLIN, 0 };
    const int r = poll(&p, 1, (int)timeout_ms);
    if (r == 0)
        return 0;
    if (r < 0)
        return errno == EINTR ? 0 : -1;

    const ssize_t n = recv(fd, buf, cap, 0);
    if (n < 0)
        return errno == ECONNREFUSED || errno == EINTR ? 0 : -1;
    return (int)n;
}

void hal_net_close(void)
{
    if (fd >= 0)
        close(fd);
    fd = -1;
}
