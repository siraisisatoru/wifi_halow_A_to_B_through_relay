/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * BSD socket portability shim for POSIX, ESP-IDF lwIP and mm-iot-sdk lwIP.
 *
 * Only the subset available on all three is used: socket/bind/listen/accept/connect,
 * send/recv/sendto/recvfrom, select, fcntl(O_NONBLOCK), setsockopt/getsockopt(SO_ERROR).
 */

#ifndef LAB_SOCK_H
#define LAB_SOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(LAB_PLATFORM_MMOSAL)
/* mm-iot-sdk: lwIP provides errno (LWIP_PROVIDE_ERRNO) and the POSIX names as macros. */
#include "lwip/sockets.h"
#define LAB_SOCK_LWIP 1
#elif defined(ESP_PLATFORM)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "lwip/sockets.h"
#define LAB_SOCK_LWIP 1
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define LAB_SOCK_LWIP 0
#endif

/* SIGPIPE suppression where the stack supports it (lwIP never raises SIGPIPE). */
#if !LAB_SOCK_LWIP && defined(MSG_NOSIGNAL)
#define LAB_SEND_FLAGS MSG_NOSIGNAL
#else
#define LAB_SEND_FLAGS 0
#endif

#define LAB_IPV4_STR_MAX 16

enum lab_wait
{
    LAB_WAIT_READ = 1,
    LAB_WAIT_WRITE = 2,
};

int lab_sock_errno(void);

/** True for EAGAIN / EWOULDBLOCK / EINTR (retry later). */
bool lab_sock_retryable(int err);

void lab_sock_close(int fd);

int lab_sock_set_nonblocking(int fd);

/** SO_REUSEADDR (best effort). */
void lab_sock_set_reuseaddr(int fd);

/** TCP_NODELAY + keepalive (best effort). */
void lab_sock_tune_tcp(int fd);

struct lab_poll
{
    /** Socket, or -1 to skip the entry. */
    int fd;
    /** Mask of @ref lab_wait to wait for. */
    int events;
    /** Set to the ready subset of @c events. */
    int revents;
};

/**
 * select() over @p n entries.
 *
 * @returns number of ready entries, 0 on timeout or EINTR, -1 on error.
 */
int lab_sock_poll(struct lab_poll *fds, size_t n, uint32_t timeout_ms);

/** lab_sock_poll() on a single socket; returns the ready mask, 0 on timeout, -1 on error. */
int lab_sock_wait(int fd, int events, uint32_t timeout_ms);

/** Parse dotted-quad IPv4 into @p sa with @p port. Returns 0 on success. */
int lab_sock_make_addr(const char *ip, uint16_t port, struct sockaddr_in *sa);

/** "a.b.c.d" for @p sa (buf >= LAB_IPV4_STR_MAX). */
const char *lab_sock_addr_str(const struct sockaddr_in *sa, char *buf, size_t len);

/** Port of @p sa in host order. */
uint16_t lab_sock_addr_port(const struct sockaddr_in *sa);

/** Same IPv4 address and port. */
bool lab_sock_addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b);

#endif /* LAB_SOCK_H */
