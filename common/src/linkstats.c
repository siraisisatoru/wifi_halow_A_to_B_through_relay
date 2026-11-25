/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lab_util.h"
#include "linkstats.h"

#if (LINKSTATS_WINDOW % 32) != 0 || (LINKSTATS_WINDOW & (LINKSTATS_WINDOW - 1)) != 0
#error "LINKSTATS_WINDOW must be a power of two and a multiple of 32"
#endif

#define SEQ_AHEAD_LIMIT 0x80000000UL

const uint32_t linkstats_hist_edges_us[LINKSTATS_HIST_BUCKETS - 1] = {
    1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000,
};

static inline uint32_t bit_index(uint32_t seq)
{
    return seq & (LINKSTATS_WINDOW - 1);
}

static inline bool bit_get(const struct linkstats *s, uint32_t seq)
{
    uint32_t i = bit_index(seq);
    return (s->window[i / 32] >> (i % 32)) & 1U;
}

static inline void bit_set(struct linkstats *s, uint32_t seq)
{
    uint32_t i = bit_index(seq);
    s->window[i / 32] |= 1UL << (i % 32);
}

static inline void bit_clear(struct linkstats *s, uint32_t seq)
{
    uint32_t i = bit_index(seq);
    s->window[i / 32] &= ~(1UL << (i % 32));
}

void linkstats_init(struct linkstats *s, const char *flow, uint64_t now_us)
{
    memset(s, 0, sizeof(*s));
    lab_strlcpy(s->flow, flow != NULL ? flow : "flow", sizeof(s->flow));
    s->start_us = now_us;
}

void linkstats_set_first_seq(struct linkstats *s, uint32_t first_seq)
{
    s->have_seq = true;
    s->highest_seq = first_seq - 1U;
    s->span = 0;
    memset(s->window, 0, sizeof(s->window));
}

void linkstats_on_sent(struct linkstats *s)
{
    s->sent++;
}

enum linkstats_rx_class linkstats_on_rx(struct linkstats *s, uint32_t seq)
{
    if (!s->have_seq)
    {
        s->have_seq = true;
        s->highest_seq = seq;
        s->span = 1;
        memset(s->window, 0, sizeof(s->window));
        bit_set(s, seq);
        s->rcvd++;
        return LINKSTATS_RX_NEW;
    }

    uint32_t ahead = seq - s->highest_seq;
    if (ahead != 0 && ahead < SEQ_AHEAD_LIMIT)
    {
        if (ahead >= LINKSTATS_WINDOW)
        {
            memset(s->window, 0, sizeof(s->window));
        }
        else
        {
            for (uint32_t k = 1; k <= ahead; k++)
            {
                bit_clear(s, s->highest_seq + k);
            }
        }
        s->lost += ahead - 1U;
        s->highest_seq = seq;
        bit_set(s, seq);
        s->span = (s->span + ahead > LINKSTATS_WINDOW || s->span + ahead < s->span) ?
                      LINKSTATS_WINDOW :
                      s->span + ahead;
        s->rcvd++;
        return LINKSTATS_RX_NEW;
    }

    uint32_t back = s->highest_seq - seq;
    if (back >= s->span)
    {
        s->stale++;
        return LINKSTATS_RX_STALE;
    }
    if (bit_get(s, seq))
    {
        s->dup++;
        return LINKSTATS_RX_DUP;
    }

    bit_set(s, seq);
    s->rcvd++;
    s->reord++;
    if (s->lost > 0)
    {
        s->lost--;
    }
    return LINKSTATS_RX_LATE;
}

static void jitter_sample(struct linkstats *s, int64_t sample_us)
{
    if (s->have_last_sample)
    {
        int64_t d = sample_us - s->last_sample_us;
        if (d < 0)
        {
            d = -d;
        }
        if (d > (int64_t)UINT32_MAX)
        {
            d = (int64_t)UINT32_MAX;
        }

        /* RFC 3550 A.8: J += |D| - J/16, kept scaled by 16. */
        int64_t j = (int64_t)s->jitter_q4 + d - (int64_t)((s->jitter_q4 + 8U) >> 4);
        if (j < 0)
        {
            j = 0;
        }
        if (j > (int64_t)UINT32_MAX)
        {
            j = (int64_t)UINT32_MAX;
        }
        s->jitter_q4 = (uint32_t)j;
    }
    s->last_sample_us = sample_us;
    s->have_last_sample = true;
}

void linkstats_on_rtt(struct linkstats *s, uint32_t rtt_us)
{
    if (s->rtt_n == 0 || rtt_us < s->rtt_min_us)
    {
        s->rtt_min_us = rtt_us;
    }
    if (rtt_us > s->rtt_max_us)
    {
        s->rtt_max_us = rtt_us;
    }
    s->rtt_n++;
    s->rtt_sum_us += rtt_us;

    size_t b = 0;
    while (b < LINKSTATS_HIST_BUCKETS - 1 && rtt_us >= linkstats_hist_edges_us[b])
    {
        b++;
    }
    s->hist[b]++;

    jitter_sample(s, (int64_t)rtt_us);
}

void linkstats_on_transit(struct linkstats *s, int64_t transit_us)
{
    jitter_sample(s, transit_us);
}

void linkstats_finalize(struct linkstats *s, uint32_t next_seq)
{
    if (!s->have_seq)
    {
        return;
    }

    uint32_t tail = next_seq - s->highest_seq - 1U;
    if (tail < SEQ_AHEAD_LIMIT)
    {
        s->lost += tail;
        s->highest_seq = next_seq - 1U;
        s->span = 0;
        memset(s->window, 0, sizeof(s->window));
    }
}

uint32_t linkstats_rtt_avg_us(const struct linkstats *s)
{
    return s->rtt_n == 0 ? 0 : (uint32_t)(s->rtt_sum_us / s->rtt_n);
}

uint32_t linkstats_jitter_us(const struct linkstats *s)
{
    return s->jitter_q4 >> 4;
}

uint32_t linkstats_loss_ppm(const struct linkstats *s)
{
    uint64_t total = (uint64_t)s->rcvd + s->lost;
    return total == 0 ? 0 : (uint32_t)(((uint64_t)s->lost * 1000000ULL) / total);
}

int linkstats_format(const struct linkstats *s,
                     uint64_t now_us,
                     const char *extra,
                     char *buf,
                     size_t len)
{
    char t_ms[24];
    uint64_t elapsed = now_us >= s->start_us ? now_us - s->start_us : 0;

    lab_u64_to_str(elapsed / 1000U, t_ms, sizeof(t_ms));

    size_t pos = 0;
    lab_appendf(buf,
                len,
                &pos,
                "STATS,%s,t_ms=%s,sent=%" PRIu32 ",rcvd=%" PRIu32 ",lost=%" PRIu32
                ",dup=%" PRIu32 ",reord=%" PRIu32 ",stale=%" PRIu32 ",loss_ppm=%" PRIu32
                ",rtt_n=%" PRIu32 ",rtt_min_us=%" PRIu32 ",rtt_avg_us=%" PRIu32
                ",rtt_max_us=%" PRIu32 ",jitter_us=%" PRIu32 ",hist=",
                s->flow,
                t_ms,
                s->sent,
                s->rcvd,
                s->lost,
                s->dup,
                s->reord,
                s->stale,
                linkstats_loss_ppm(s),
                s->rtt_n,
                s->rtt_min_us,
                linkstats_rtt_avg_us(s),
                s->rtt_max_us,
                linkstats_jitter_us(s));
    for (size_t i = 0; i < LINKSTATS_HIST_BUCKETS; i++)
    {
        lab_appendf(buf, len, &pos, i == 0 ? "%" PRIu32 : "/%" PRIu32, s->hist[i]);
    }
    if (extra != NULL)
    {
        lab_appendf(buf, len, &pos, "%s", extra);
    }
    return pos < len ? (int)pos : -1;
}
