/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Bulk throughput test (iperf-like), ESP-IDF only. The client sends as fast as it can for a fixed
 * time; the server counts what arrives. The server's numbers are the result.
 *
 * Server lines (t_ms = ms since boot):
 *
 *   STATS,tput_srv,t_ms=,proto=tcp|udp,test=,interval_ms=,bytes=,kbps=[,pkts=,lost=]
 *   EVENT,tput_srv,t_ms=,ev=end,proto=,test=,ms=,bytes=,kbps=[,pkts=,lost=,loss_pct=]
 *
 * Client lines:
 *
 *   EVENT,tput,t_ms=,ev=start,proto=,test=,dst=,duration_ms=,len=
 *   EVENT,tput,t_ms=,ev=end,proto=,test=,ms=,bytes=,kbps=[,pkts=,send_err=]
 *
 * kbps are payload bits (TCP stream bytes or UDP payload bytes) per second, 1 kbps = 1000 bit/s.
 * UDP datagrams carry a 16-byte header (magic, test id, sequence number) in the payload, so the
 * server also reports datagrams lost. A UDP test ends at the server when its end marker arrives or
 * after 2 s without data.
 */

#ifndef TPUT_H
#define TPUT_H

#include <stdbool.h>
#include <stdint.h>

#define TPUT_UDP_MAX_LEN 1472

struct tput_server_cfg
{
    uint16_t tcp_port;
    uint16_t udp_port;
    uint32_t report_interval_ms;
};

/** Start the TCP and UDP sink tasks. Returns 0 on success. */
int tput_server_start(const struct tput_server_cfg *cfg);

enum tput_proto
{
    TPUT_TCP,
    TPUT_UDP,
};

struct tput_client_cfg
{
    enum tput_proto proto;
    const char *dst_ip;
    uint16_t port;
    uint32_t duration_ms;
    /** Bytes per send() (TCP) or per datagram (UDP, at least 16, at most TPUT_UDP_MAX_LEN). */
    uint32_t len;
    /** Polled; the test ends early when it becomes true (may be NULL). */
    const volatile bool *stop;
};

/** Run one test in the calling task. Returns the measured kbps (sender side), or -1 on error. */
int32_t tput_client_run(const struct tput_client_cfg *cfg);

/**
 * Alternate TCP and UDP tests to @p dst_ip (ports LAB_TPUT_TCP_PORT / LAB_TPUT_UDP_PORT), each
 * @p duration_ms long followed by a @p pause_ms pause, until @p stop becomes true (forever if
 * @p stop is NULL). TCP writes 1460-byte buffers; UDP datagrams carry @p udp_len bytes.
 */
void tput_client_loop(const char *dst_ip, uint32_t duration_ms, uint32_t pause_ms, uint32_t udp_len,
                      const volatile bool *stop);

#endif /* TPUT_H */
