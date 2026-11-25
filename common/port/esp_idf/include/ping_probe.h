/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Continuous ICMP echo to one target over an lwIP raw socket connected to that target, printed
 * as machine-readable lines (t_ms = ms since boot):
 *
 *   PING,<name>,t_ms=,ev=reply,dst=,seq=,rtt_ms=,ttl=,size=
 *   PING,<name>,t_ms=,ev=timeout,dst=,seq=
 *   PING,<name>,t_ms=,ev=summary,dst=,sent=,rcvd=,lost=,loss_ppm=,rtt_min_ms=,rtt_avg_ms=,
 *        rtt_max_ms=,running=
 *
 * reply/timeout lines are optional (verbose). Summary counters are cumulative since boot, across
 * stop/start; a summary is printed every report interval and when the probe stops. `seq` restarts
 * at 1 on every start (and wraps from 65535 to 1), `size` is the ICMP data length (as esp_ping
 * reports it) and RTT is in whole milliseconds. A reply that arrives after `timeout_ms` counts as
 * a timeout.
 */

#ifndef PING_PROBE_H
#define PING_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

struct ping_probe
{
    /* Configuration (set before the first ping_probe_start()). */
    const char *name;
    const char *dst;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint32_t data_size;
    uint32_t report_interval_ms;
    bool verbose;

    /* Runtime state. The counters are only written by the probe's task. */
    uint16_t echo_id;
    volatile bool stop;
    volatile bool active;
    uint32_t sent;
    uint32_t rcvd;
    uint32_t rtt_min_ms;
    uint32_t rtt_max_ms;
    uint64_t rtt_sum_ms;
    uint32_t next_summary_ms;
};

/**
 * Start the probe's task. Returns ESP_ERR_INVALID_STATE while the task of a previous run has not
 * exited yet (see ping_probe_is_active()).
 */
esp_err_t ping_probe_start(struct ping_probe *probe);

/** Ask the probe to stop. Returns at once; the task exits within about 100 ms. */
void ping_probe_stop(struct ping_probe *probe);

/** True from ping_probe_start() until the probe's task has printed its final summary. */
bool ping_probe_is_active(const struct ping_probe *probe);

#endif /* PING_PROBE_H */
