/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Blocking traffic-service loops over BSD sockets (POSIX, ESP-IDF lwIP, mm-iot-sdk lwIP).
 *
 * Each *_run() call blocks the calling task until it finishes or @c *stop becomes true (polled
 * at least every LAB_SVC_POLL_MS). Output: STATS and EVENT lines via lab_out_line(), see
 * common/README.md. Working buffers (a few KiB) are allocated once per call with lab_malloc();
 * nothing is allocated per packet. Recommended task stack: 6 KiB.
 *
 * Call lab_platform_init() once before using any service.
 */

#ifndef LAB_SERVICES_H
#define LAB_SERVICES_H

#include <stdbool.h>
#include <stdint.h>

#include "linkstats.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Upper bound on how long a service takes to notice @c *stop. */
#define LAB_SVC_POLL_MS 100

/** Upper bound for tcp_stream_client_cfg.max_inflight. */
#define LAB_TCP_MAX_INFLIGHT_LIMIT 128

enum lab_svc_result
{
    LAB_SVC_OK = 0,
    /** Invalid configuration (bad IP string, payload too large, zero rate...). */
    LAB_SVC_ERR_CFG = -1,
    /** socket()/bind()/listen() failed. */
    LAB_SVC_ERR_SOCKET = -2,
    LAB_SVC_ERR_NOMEM = -3,
};

struct udp_echo_server_cfg
{
    uint16_t port;
    /** Period of the per-session uplink STATS line; 0 disables periodic reports. */
    uint32_t report_interval_ms;
    /** Flow name in STATS/EVENT lines (default "udp_srv"). */
    const char *flow;
    /** Stop request (may be NULL: run forever). */
    const volatile bool *stop;
};

struct udp_echo_client_cfg
{
    /** Destination IPv4 dotted quad (default LAB_IP_NODE_B). */
    const char *dst_ip;
    uint16_t dst_port;
    /** Requests per second (1..10000). */
    uint32_t rate_pps;
    /** Payload bytes after the linkproto header (0..LINKPROTO_MAX_PAYLOAD). */
    uint16_t payload_len;
    /** Period of STATS lines; 0 = only the final one. */
    uint32_t report_interval_ms;
    /** Requests to send; 0 = unlimited. */
    uint32_t count;
    /** Run time; 0 = unlimited. */
    uint32_t duration_ms;
    /** Wait for late echoes after the last request (not applied when stopped). */
    uint32_t drain_ms;
    /** Flow name (default "udp"). */
    const char *flow;
    const volatile bool *stop;
};

struct tcp_stream_server_cfg
{
    uint16_t port;
    uint32_t report_interval_ms;
    /** Close a connection that delivered nothing for this long; 0 = never. */
    uint32_t idle_timeout_ms;
    /** Flow name (default "tcp_srv"). */
    const char *flow;
    const volatile bool *stop;
};

struct tcp_stream_client_cfg
{
    const char *dst_ip;
    uint16_t dst_port;
    /** Messages per second (1..10000). */
    uint32_t rate_mps;
    /** Message payload bytes (0..MSGSTREAM_MAX_PAYLOAD). */
    uint16_t payload_len;
    uint32_t report_interval_ms;
    /** Messages to send; 0 = unlimited. */
    uint32_t count;
    /** Run time; 0 = unlimited. */
    uint32_t duration_ms;
    uint32_t connect_timeout_ms;
    /** Oldest unacked message older than this => stall => reconnect. Also final ack wait. */
    uint32_t ack_timeout_ms;
    /** Reconnect delay: min * 2^(attempt-1) capped at max, then randomised to [d/2, d]. */
    uint32_t backoff_min_ms;
    uint32_t backoff_max_ms;
    /** Outstanding message window (1..LAB_TCP_MAX_INFLIGHT_LIMIT). */
    uint32_t max_inflight;
    /** Flow name (default "tcp"). */
    const char *flow;
    const volatile bool *stop;
};

/** Fill with defaults from halow_lab_config.h. */
void udp_echo_server_cfg_init(struct udp_echo_server_cfg *cfg);
void udp_echo_client_cfg_init(struct udp_echo_client_cfg *cfg);
void tcp_stream_server_cfg_init(struct tcp_stream_server_cfg *cfg);
void tcp_stream_client_cfg_init(struct tcp_stream_client_cfg *cfg);

/**
 * Echo every valid ECHO_REQ back to its sender as ECHO_RSP (same seq/session/ts/payload).
 * Tracks the uplink (requests) per client session: loss/dup/reorder and one-way jitter.
 * Runs until stopped.
 */
int udp_echo_server_run(const struct udp_echo_server_cfg *cfg);

/**
 * Send seq-numbered timestamped ECHO_REQs at @c rate_pps and account the echoes (round-trip
 * loss, duplicates, reordering, RTT, jitter).
 *
 * @param result    Optional: receives the final statistics.
 */
int udp_echo_client_run(const struct udp_echo_client_cfg *cfg, struct linkstats *result);

/**
 * Accept stream connections (a newer connection replaces the current one, so a client that
 * reconnects after a relay drop is served immediately), validate each MSG and answer MSG_ACK.
 * Runs until stopped.
 */
int tcp_stream_server_run(const struct tcp_stream_server_cfg *cfg);

/**
 * Keep a connection to the stream server, send MSGs at @c rate_mps, account the ACKs, detect
 * stalls and reconnect with exponential backoff and jitter. Sequence numbers continue across
 * reconnects (one session per run).
 *
 * @param result    Optional: receives the final statistics.
 */
int tcp_stream_client_run(const struct tcp_stream_client_cfg *cfg, struct linkstats *result);

#ifdef __cplusplus
}
#endif

#endif /* LAB_SERVICES_H */
