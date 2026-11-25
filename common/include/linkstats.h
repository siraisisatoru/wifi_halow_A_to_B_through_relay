/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Per-flow sequence and latency statistics with a one-line machine-parseable report.
 *
 * Sequence accounting (serial-number arithmetic, so it survives the 2^32 wrap):
 *  - A seq ahead of the highest seen advances the window; every skipped seq counts as lost.
 *  - A seq behind the highest that was not yet seen is a late arrival: reord += 1, lost -= 1.
 *  - A seq behind the highest that was already seen is a duplicate.
 *  - A seq further behind than the tracked window (or older than the flow) is "stale" and
 *    otherwise ignored.
 * Lost therefore only counts gaps below the highest seq seen; call linkstats_finalize() at the
 * end of a run to also count the tail.
 *
 * Not thread-safe: update and format a flow from one task.
 */

#ifndef LINKSTATS_H
#define LINKSTATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Duplicate/reorder detection window in sequence numbers (power of two, multiple of 32). */
#ifndef LINKSTATS_WINDOW
#define LINKSTATS_WINDOW 1024
#endif

#define LINKSTATS_FLOW_NAME_MAX 16

/** RTT histogram: bucket i counts rtt < linkstats_hist_edges_us[i]; last bucket is the rest. */
#define LINKSTATS_HIST_BUCKETS 12

extern const uint32_t linkstats_hist_edges_us[LINKSTATS_HIST_BUCKETS - 1];

enum linkstats_rx_class
{
    /** First packet, in order, or ahead of the highest seq (gap counted as lost). */
    LINKSTATS_RX_NEW,
    /** Behind the highest seq and not seen before (fills a gap). */
    LINKSTATS_RX_LATE,
    LINKSTATS_RX_DUP,
    /** Too old to classify (outside the window or before the flow started). */
    LINKSTATS_RX_STALE,
};

struct linkstats
{
    char flow[LINKSTATS_FLOW_NAME_MAX];
    uint64_t start_us;

    uint32_t sent;
    /** Unique packets received (NEW + LATE). */
    uint32_t rcvd;
    uint32_t lost;
    uint32_t dup;
    uint32_t reord;
    uint32_t stale;

    uint32_t rtt_n;
    uint32_t rtt_min_us;
    uint32_t rtt_max_us;
    uint64_t rtt_sum_us;
    uint32_t hist[LINKSTATS_HIST_BUCKETS];

    /** RFC 3550 A.8 jitter estimate scaled by 16. */
    uint32_t jitter_q4;
    int64_t last_sample_us;
    bool have_last_sample;

    bool have_seq;
    uint32_t highest_seq;
    /** Number of seqs ending at highest_seq tracked in the bitmap (<= LINKSTATS_WINDOW). */
    uint32_t span;
    uint32_t window[LINKSTATS_WINDOW / 32];
};

/** Reset all counters. @p flow is copied (truncated to LINKSTATS_FLOW_NAME_MAX - 1). */
void linkstats_init(struct linkstats *s, const char *flow, uint64_t now_us);

/**
 * Declare the first seq the sender will use, so that losses before the first arrival are
 * counted. Without it the first received seq starts the flow.
 */
void linkstats_set_first_seq(struct linkstats *s, uint32_t first_seq);

void linkstats_on_sent(struct linkstats *s);

/** Account a received seq; the result tells the caller whether to also take an RTT sample. */
enum linkstats_rx_class linkstats_on_rx(struct linkstats *s, uint32_t seq);

/** RTT sample: min/avg/max, histogram, and jitter from consecutive RTTs (arrival order). */
void linkstats_on_rtt(struct linkstats *s, uint32_t rtt_us);

/**
 * One-way transit sample (receiver clock minus sender timestamp, any constant offset). Updates
 * only the jitter estimate. Use either this or linkstats_on_rtt() for a given flow.
 */
void linkstats_on_transit(struct linkstats *s, int64_t transit_us);

/** Count every seq after the highest received and before @p next_seq as lost. */
void linkstats_finalize(struct linkstats *s, uint32_t next_seq);

uint32_t linkstats_rtt_avg_us(const struct linkstats *s);

uint32_t linkstats_jitter_us(const struct linkstats *s);

/** lost / (rcvd + lost) in parts per million. */
uint32_t linkstats_loss_ppm(const struct linkstats *s);

/**
 * Format the STATS line (no trailing newline):
 *
 *   STATS,<flow>,t_ms=,sent=,rcvd=,lost=,dup=,reord=,stale=,loss_ppm=,rtt_n=,rtt_min_us=,
 *   rtt_avg_us=,rtt_max_us=,jitter_us=,hist=<c0>/<c1>/.../<c11><extra>
 *
 * @param extra     Appended verbatim (NULL for none); should start with ',' and contain
 *                  further key=value pairs.
 *
 * @returns the length written (excluding NUL), or -1 if @p buf was too small (it is then
 *          still NUL-terminated).
 */
int linkstats_format(const struct linkstats *s,
                     uint64_t now_us,
                     const char *extra,
                     char *buf,
                     size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LINKSTATS_H */
