/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "lab_sock.h"

int lab_sock_errno(void)
{
    return errno;
}

bool lab_sock_retryable(int err)
{
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (err == EWOULDBLOCK)
    {
        return true;
    }
#endif
    return err == EAGAIN || err == EINTR;
}

void lab_sock_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}

int lab_sock_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        flags = 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

void lab_sock_set_reuseaddr(int fd)
{
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

void lab_sock_tune_tcp(int fd)
{
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    int idle_s = 10;
    int intvl_s = 5;
    int cnt = 3;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof(idle_s));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl_s, sizeof(intvl_s));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

int lab_sock_poll(struct lab_poll *fds, size_t n, uint32_t timeout_ms)
{
    fd_set rd;
    fd_set wr;
    int maxfd = -1;

    FD_ZERO(&rd);
    FD_ZERO(&wr);
    for (size_t i = 0; i < n; i++)
    {
        fds[i].revents = 0;
        if (fds[i].fd < 0)
        {
            continue;
        }
        if (fds[i].events & LAB_WAIT_READ)
        {
            FD_SET(fds[i].fd, &rd);
        }
        if (fds[i].events & LAB_WAIT_WRITE)
        {
            FD_SET(fds[i].fd, &wr);
        }
        if (fds[i].fd > maxfd)
        {
            maxfd = fds[i].fd;
        }
    }

    struct timeval tv;
    tv.tv_sec = (long)(timeout_ms / 1000U);
    tv.tv_usec = (long)(timeout_ms % 1000U) * 1000L;

    int rc = select(maxfd + 1, &rd, &wr, NULL, &tv);
    if (rc < 0)
    {
        return lab_sock_errno() == EINTR ? 0 : -1;
    }
    if (rc == 0)
    {
        return 0;
    }

    int ready = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (fds[i].fd < 0)
        {
            continue;
        }
        if ((fds[i].events & LAB_WAIT_READ) && FD_ISSET(fds[i].fd, &rd))
        {
            fds[i].revents |= LAB_WAIT_READ;
        }
        if ((fds[i].events & LAB_WAIT_WRITE) && FD_ISSET(fds[i].fd, &wr))
        {
            fds[i].revents |= LAB_WAIT_WRITE;
        }
        if (fds[i].revents != 0)
        {
            ready++;
        }
    }
    return ready;
}

int lab_sock_wait(int fd, int events, uint32_t timeout_ms)
{
    struct lab_poll p = { .fd = fd, .events = events, .revents = 0 };
    int rc = lab_sock_poll(&p, 1, timeout_ms);
    return rc < 0 ? -1 : p.revents;
}

int lab_sock_make_addr(const char *ip, uint16_t port, struct sockaddr_in *sa)
{
    uint8_t octets[4];
    const char *p = ip;

    if (ip == NULL || sa == NULL)
    {
        return -1;
    }
    for (int i = 0; i < 4; i++)
    {
        unsigned v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4)
        {
            v = v * 10U + (unsigned)(*p - '0');
            p++;
            digits++;
        }
        if (digits == 0 || digits > 3 || v > 255)
        {
            return -1;
        }
        octets[i] = (uint8_t)v;
        if (i < 3)
        {
            if (*p != '.')
            {
                return -1;
            }
            p++;
        }
    }
    if (*p != '\0')
    {
        return -1;
    }

    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    memcpy(&sa->sin_addr.s_addr, octets, sizeof(octets));
    return 0;
}

const char *lab_sock_addr_str(const struct sockaddr_in *sa, char *buf, size_t len)
{
    uint8_t o[4];
    memcpy(o, &sa->sin_addr.s_addr, sizeof(o));
    snprintf(buf, len, "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
    return buf;
}

uint16_t lab_sock_addr_port(const struct sockaddr_in *sa)
{
    return ntohs(sa->sin_port);
}

bool lab_sock_addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_port == b->sin_port &&
           memcmp(&a->sin_addr.s_addr, &b->sin_addr.s_addr, sizeof(a->sin_addr.s_addr)) == 0;
}
