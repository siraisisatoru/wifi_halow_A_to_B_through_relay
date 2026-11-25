/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include "linkstats.h"
#include "test_harness.h"

static struct linkstats s;

static void rx_seq(const uint32_t *seqs, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        (void)linkstats_on_rx(&s, seqs[i]);
    }
}

static void test_in_order(void)
{
    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0);
    for (uint32_t i = 0; i < 5000; i++)
    {
        linkstats_on_sent(&s);
        CHECK_EQ(linkstats_on_rx(&s, i), LINKSTATS_RX_NEW);
    }
    CHECK_EQ(s.sent, 5000);
    CHECK_EQ(s.rcvd, 5000);
    CHECK_EQ(s.lost, 0);
    CHECK_EQ(s.dup, 0);
    CHECK_EQ(s.reord, 0);
    CHECK_EQ(s.stale, 0);
    CHECK_EQ(linkstats_loss_ppm(&s), 0);
}

static void test_gap_and_late_correction(void)
{
    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0);
    static const uint32_t seqs[] = { 0, 1, 2, 5, 6 };
    rx_seq(seqs, 5);
    CHECK_EQ(s.lost, 2);
    CHECK_EQ(s.rcvd, 5);

    CHECK_EQ(linkstats_on_rx(&s, 3), LINKSTATS_RX_LATE);
    CHECK_EQ(s.lost, 1);
    CHECK_EQ(s.reord, 1);
    CHECK_EQ(linkstats_on_rx(&s, 4), LINKSTATS_RX_LATE);
    CHECK_EQ(s.lost, 0);
    CHECK_EQ(s.reord, 2);
    CHECK_EQ(s.rcvd, 7);

    /* The same late packet again is a duplicate, not another correction. */
    CHECK_EQ(linkstats_on_rx(&s, 4), LINKSTATS_RX_DUP);
    CHECK_EQ(s.dup, 1);
    CHECK_EQ(s.lost, 0);
    CHECK_EQ(s.rcvd, 7);
}

static void test_duplicates(void)
{
    linkstats_init(&s, "t", 0);
    static const uint32_t seqs[] = { 10, 11, 11, 10, 12, 12, 12 };
    rx_seq(seqs, 7);
    CHECK_EQ(s.rcvd, 3);
    CHECK_EQ(s.dup, 4);
    CHECK_EQ(s.lost, 0);
    CHECK_EQ(s.reord, 0);
}

static void test_first_seq_losses(void)
{
    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0);
    CHECK_EQ(linkstats_on_rx(&s, 3), LINKSTATS_RX_NEW);
    CHECK_EQ(s.lost, 3);
    CHECK_EQ(linkstats_on_rx(&s, 0), LINKSTATS_RX_LATE);
    CHECK_EQ(s.lost, 2);
    /* Before the declared first seq. */
    CHECK_EQ(linkstats_on_rx(&s, 0xFFFFFFFFUL), LINKSTATS_RX_STALE);
    CHECK_EQ(s.stale, 1);

    /* Without a declared first seq the first arrival starts the flow. */
    linkstats_init(&s, "t", 0);
    CHECK_EQ(linkstats_on_rx(&s, 100), LINKSTATS_RX_NEW);
    CHECK_EQ(linkstats_on_rx(&s, 99), LINKSTATS_RX_STALE);
    CHECK_EQ(s.lost, 0);
    CHECK_EQ(s.rcvd, 1);
}

static void test_window_and_stale(void)
{
    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0);
    CHECK_EQ(linkstats_on_rx(&s, 0), LINKSTATS_RX_NEW);
    CHECK_EQ(linkstats_on_rx(&s, 1500), LINKSTATS_RX_NEW);
    CHECK_EQ(s.lost, 1499);

    /* Inside the window behind the new highest: late. */
    CHECK_EQ(linkstats_on_rx(&s, 1500 - (LINKSTATS_WINDOW - 1)), LINKSTATS_RX_LATE);
    CHECK_EQ(s.lost, 1498);
    /* Exactly one window behind: no longer tracked. */
    CHECK_EQ(linkstats_on_rx(&s, 1500 - LINKSTATS_WINDOW), LINKSTATS_RX_STALE);
    CHECK_EQ(linkstats_on_rx(&s, 0), LINKSTATS_RX_STALE);
    CHECK_EQ(s.stale, 2);
    CHECK_EQ(s.lost, 1498);

    /* Window slides: bits of old seqs that map to the same slot must not look like dups. */
    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0);
    for (uint32_t i = 0; i < 3 * LINKSTATS_WINDOW; i += 2)
    {
        CHECK_EQ(linkstats_on_rx(&s, i), LINKSTATS_RX_NEW);
    }
    uint32_t highest = 3 * LINKSTATS_WINDOW - 2;
    CHECK_EQ(linkstats_on_rx(&s, highest - 1), LINKSTATS_RX_LATE);
    CHECK_EQ(linkstats_on_rx(&s, highest - 2), LINKSTATS_RX_DUP);
    CHECK_EQ(s.lost, (3 * LINKSTATS_WINDOW) / 2 - 1 - 1);
}

static void test_wraparound(void)
{
    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0xFFFFFFF0UL);
    for (uint32_t i = 0; i < 33; i++)
    {
        CHECK_EQ(linkstats_on_rx(&s, 0xFFFFFFF0UL + i), LINKSTATS_RX_NEW);
    }
    CHECK_EQ(s.rcvd, 33);
    CHECK_EQ(s.lost, 0);
    CHECK_EQ(s.highest_seq, 0x10);

    /* Gap spanning the wrap, then the late packets across it. */
    linkstats_init(&s, "t", 0);
    CHECK_EQ(linkstats_on_rx(&s, 0xFFFFFFFEUL), LINKSTATS_RX_NEW);
    CHECK_EQ(linkstats_on_rx(&s, 1), LINKSTATS_RX_NEW);
    CHECK_EQ(s.lost, 2);
    CHECK_EQ(linkstats_on_rx(&s, 0), LINKSTATS_RX_LATE);
    CHECK_EQ(linkstats_on_rx(&s, 0xFFFFFFFFUL), LINKSTATS_RX_LATE);
    CHECK_EQ(s.lost, 0);
    CHECK_EQ(s.reord, 2);
    CHECK_EQ(linkstats_on_rx(&s, 0xFFFFFFFEUL), LINKSTATS_RX_DUP);
    CHECK_EQ(linkstats_on_rx(&s, 0), LINKSTATS_RX_DUP);
    CHECK_EQ(s.dup, 2);

    /* Finalize across the wrap. */
    linkstats_finalize(&s, 5);
    CHECK_EQ(s.lost, 3);
}

static void test_finalize(void)
{
    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0);
    for (uint32_t i = 0; i < 7; i++)
    {
        (void)linkstats_on_rx(&s, i);
    }
    linkstats_finalize(&s, 10);
    CHECK_EQ(s.lost, 3);
    CHECK_EQ(linkstats_loss_ppm(&s), 300000);

    /* Nothing received at all. */
    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0);
    linkstats_finalize(&s, 5);
    CHECK_EQ(s.lost, 5);
    CHECK_EQ(linkstats_loss_ppm(&s), 1000000);

    /* next_seq not beyond highest: no change. */
    linkstats_init(&s, "t", 0);
    (void)linkstats_on_rx(&s, 50);
    linkstats_finalize(&s, 51);
    linkstats_finalize(&s, 10);
    CHECK_EQ(s.lost, 0);

    /* Never started: no-op. */
    linkstats_init(&s, "t", 0);
    linkstats_finalize(&s, 100);
    CHECK_EQ(s.lost, 0);
}

static void test_rtt_and_histogram(void)
{
    linkstats_init(&s, "t", 0);
    linkstats_on_rtt(&s, 1000);
    linkstats_on_rtt(&s, 3000);
    linkstats_on_rtt(&s, 2000);
    CHECK_EQ(s.rtt_n, 3);
    CHECK_EQ(s.rtt_min_us, 1000);
    CHECK_EQ(s.rtt_max_us, 3000);
    CHECK_EQ(linkstats_rtt_avg_us(&s), 2000);

    /* Bucket i holds rtt < edge[i]; an exact edge goes to the next bucket. */
    CHECK_EQ(s.hist[0], 0);
    CHECK_EQ(s.hist[1], 1);
    CHECK_EQ(s.hist[2], 2);

    linkstats_on_rtt(&s, 0);
    linkstats_on_rtt(&s, 999);
    linkstats_on_rtt(&s, 2000000);
    linkstats_on_rtt(&s, UINT32_MAX);
    CHECK_EQ(s.hist[0], 2);
    CHECK_EQ(s.hist[LINKSTATS_HIST_BUCKETS - 1], 2);
    CHECK_EQ(s.rtt_min_us, 0);
    CHECK_EQ(s.rtt_max_us, UINT32_MAX);

    uint32_t total = 0;
    for (size_t i = 0; i < LINKSTATS_HIST_BUCKETS; i++)
    {
        total += s.hist[i];
    }
    CHECK_EQ(total, s.rtt_n);

    linkstats_init(&s, "t", 0);
    CHECK_EQ(linkstats_rtt_avg_us(&s), 0);
}

static void test_jitter(void)
{
    /* Constant delay: no jitter. */
    linkstats_init(&s, "t", 0);
    for (int i = 0; i < 100; i++)
    {
        linkstats_on_rtt(&s, 5000);
    }
    CHECK_EQ(linkstats_jitter_us(&s), 0);

    /* RFC 3550 A.8 first step: J = |D| / 16. */
    linkstats_init(&s, "t", 0);
    linkstats_on_rtt(&s, 1000);
    linkstats_on_rtt(&s, 2000);
    CHECK_EQ(s.jitter_q4, 1000);
    CHECK_EQ(linkstats_jitter_us(&s), 62);

    /* Alternating delay converges to |D|. */
    for (int i = 0; i < 400; i++)
    {
        linkstats_on_rtt(&s, (i & 1) ? 2000 : 1000);
    }
    uint32_t j = linkstats_jitter_us(&s);
    CHECK(j >= 990 && j <= 1000);

    /* Compare against a floating-point reference on a pseudo-random series. */
    linkstats_init(&s, "t", 0);
    double ref = 0.0;
    int64_t prev = 0;
    for (int i = 0; i < 2000; i++)
    {
        int64_t transit = -5000000 + (int64_t)(test_rand() % 20000);
        linkstats_on_transit(&s, transit);
        if (i > 0)
        {
            double d = (double)llabs(transit - prev);
            ref += (d - ref) / 16.0;
        }
        prev = transit;
    }
    double got = (double)linkstats_jitter_us(&s);
    CHECK(got > ref - 5.0 && got < ref + 5.0);
    CHECK_EQ(s.rtt_n, 0);
}

static void test_format(void)
{
    char line[512];

    linkstats_init(&s, "udp", 1000000);
    linkstats_set_first_seq(&s, 0);
    for (uint32_t i = 0; i < 10; i++)
    {
        linkstats_on_sent(&s);
        if (i != 4)
        {
            (void)linkstats_on_rx(&s, i);
            linkstats_on_rtt(&s, 1500 + i);
        }
    }
    (void)linkstats_on_rx(&s, 9);
    int n = linkstats_format(&s, 1000000 + 12345678, ",final=1", line, sizeof(line));
    CHECK(n > 0);
    CHECK_EQ((size_t)n, strlen(line));
    CHECK(strncmp(line, "STATS,udp,t_ms=12345,sent=10,rcvd=9,lost=1,dup=1,reord=0,stale=0,"
                        "loss_ppm=100000,rtt_n=9,rtt_min_us=1500,rtt_avg_us=1504,"
                        "rtt_max_us=1509,jitter_us=",
                  strlen("STATS,udp,t_ms=12345,sent=10,rcvd=9,lost=1,dup=1,reord=0,stale=0,"
                         "loss_ppm=100000,rtt_n=9,rtt_min_us=1500,rtt_avg_us=1504,"
                         "rtt_max_us=1509,jitter_us=")) == 0);
    CHECK(strstr(line, ",hist=0/9/0/0/0/0/0/0/0/0/0/0,final=1") != NULL);
    CHECK(strchr(line, '\n') == NULL);
    CHECK(strchr(line, ' ') == NULL);

    /* 64-bit t_ms. */
    linkstats_init(&s, "x", 0);
    n = linkstats_format(&s, 5000000000000ULL * 1000ULL, NULL, line, sizeof(line));
    CHECK(n > 0);
    CHECK(strstr(line, "t_ms=5000000000000,") != NULL);

    /* Truncation: -1 and still NUL-terminated. */
    memset(line, 'Z', sizeof(line));
    n = linkstats_format(&s, 0, NULL, line, 20);
    CHECK_EQ(n, -1);
    CHECK_EQ(strlen(line), 19);

    /* Clock earlier than start: t_ms clamps to 0. */
    linkstats_init(&s, "x", 1000);
    (void)linkstats_format(&s, 0, NULL, line, sizeof(line));
    CHECK(strstr(line, "t_ms=0,") != NULL);
}

static void test_flow_name(void)
{
    linkstats_init(&s, "a_very_long_flow_name_that_is_truncated", 0);
    CHECK_EQ(strlen(s.flow), LINKSTATS_FLOW_NAME_MAX - 1);
    linkstats_init(&s, NULL, 0);
    CHECK(strcmp(s.flow, "flow") == 0);
}

static void test_random_reorder_conservation(void)
{
    /* Deliver 0..N-1 with random local swaps, drops and dups; check the invariants. */
    enum
    {
        N = 20000
    };
    static uint32_t order[N];
    static uint8_t delivered[N];

    for (uint32_t i = 0; i < N; i++)
    {
        order[i] = i;
    }
    for (uint32_t i = 0; i + 8 < N; i++)
    {
        if (test_rand() % 4 == 0)
        {
            uint32_t j = i + 1 + test_rand() % 8;
            uint32_t t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
    }

    linkstats_init(&s, "t", 0);
    linkstats_set_first_seq(&s, 0);
    memset(delivered, 0, sizeof(delivered));
    uint32_t unique = 0;
    uint32_t dups = 0;
    for (uint32_t i = 0; i < N; i++)
    {
        if (test_rand() % 10 == 0)
        {
            continue;
        }
        uint32_t seq = order[i];
        (void)linkstats_on_rx(&s, seq);
        if (!delivered[seq])
        {
            delivered[seq] = 1;
            unique++;
        }
        if (test_rand() % 20 == 0)
        {
            (void)linkstats_on_rx(&s, seq);
            dups++;
        }
    }
    linkstats_finalize(&s, N);
    CHECK_EQ(s.rcvd, unique);
    CHECK_EQ(s.dup, dups);
    CHECK_EQ(s.stale, 0);
    CHECK_EQ(s.rcvd + s.lost, N);
    CHECK(s.reord > 0);
}

static const struct test_case cases[] = {
    { "linkstats/in_order", test_in_order },
    { "linkstats/gap_and_late_correction", test_gap_and_late_correction },
    { "linkstats/duplicates", test_duplicates },
    { "linkstats/first_seq_losses", test_first_seq_losses },
    { "linkstats/window_and_stale", test_window_and_stale },
    { "linkstats/wraparound", test_wraparound },
    { "linkstats/finalize", test_finalize },
    { "linkstats/rtt_and_histogram", test_rtt_and_histogram },
    { "linkstats/jitter", test_jitter },
    { "linkstats/format", test_format },
    { "linkstats/flow_name", test_flow_name },
    { "linkstats/random_reorder_conservation", test_random_reorder_conservation },
};

void test_linkstats_register(const struct test_case **out, size_t *n)
{
    *out = cases;
    *n = sizeof(cases) / sizeof(cases[0]);
}
