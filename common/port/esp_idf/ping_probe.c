/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ICMP echo prober, one task per target.
 *
 * esp_ping is not used: its raw sockets are not connected, so with two probes running every
 * echo reply reaches both sockets, and each foreign reply restarts the other probe's full receive
 * timeout. Here every probe connect()s its raw socket to its target (lwIP then delivers only
 * ICMP from that address to it) and waits for the reply against a fixed deadline.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/sockets.h"

#include "lab_platform.h"
#include "ping_probe.h"

static const char *TAG = "ping";

/* The task formats lines on its own stack. */
#define PING_TASK_STACK_BYTES 4096
#define PING_TASK_PRIO        4

/* How often a waiting probe checks for a stop request. */
#define PING_POLL_MS 100

#define PING_MAX_DATA 1400
#define PING_TTL      64

static uint16_t next_echo_id = 0x4c41;

/* Milliseconds since boot, the t_ms of every line (wraps after ~49 days). */
static uint32_t uptime_ms(void)
{
    return (uint32_t)(lab_time_us() / 1000);
}

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void print_summary(struct ping_probe *probe)
{
    uint32_t lost = probe->sent - probe->rcvd;
    uint32_t loss_ppm = 0;
    uint32_t avg = 0;

    if (probe->sent > 0)
    {
        loss_ppm = (uint32_t)(((uint64_t)lost * 1000000u) / probe->sent);
    }
    if (probe->rcvd > 0)
    {
        avg = (uint32_t)(probe->rtt_sum_ms / probe->rcvd);
    }

    lab_outf("PING,%s,t_ms=%" PRIu32 ",ev=summary,dst=%s,sent=%" PRIu32 ",rcvd=%" PRIu32
             ",lost=%" PRIu32 ",loss_ppm=%" PRIu32 ",rtt_min_ms=%" PRIu32 ",rtt_avg_ms=%" PRIu32
             ",rtt_max_ms=%" PRIu32 ",running=%d",
             probe->name,
             uptime_ms(),
             probe->dst,
             probe->sent,
             probe->rcvd,
             lost,
             loss_ppm,
             (probe->rcvd > 0) ? probe->rtt_min_ms : 0,
             avg,
             probe->rtt_max_ms,
             probe->stop ? 0 : 1);
}

static void maybe_print_summary(struct ping_probe *probe)
{
    if (probe->report_interval_ms == 0)
    {
        return;
    }

    uint32_t now = uptime_ms();
    if (time_reached(now, probe->next_summary_ms))
    {
        probe->next_summary_ms = now + probe->report_interval_ms;
        print_summary(probe);
    }
}

static void record_reply(struct ping_probe *probe, uint16_t seq, uint32_t rtt_ms, uint8_t ttl,
                         uint32_t size)
{
    probe->sent++;
    probe->rcvd++;
    probe->rtt_sum_ms += rtt_ms;
    if (probe->rcvd == 1 || rtt_ms < probe->rtt_min_ms)
    {
        probe->rtt_min_ms = rtt_ms;
    }
    if (rtt_ms > probe->rtt_max_ms)
    {
        probe->rtt_max_ms = rtt_ms;
    }

    if (probe->verbose)
    {
        lab_outf("PING,%s,t_ms=%" PRIu32 ",ev=reply,dst=%s,seq=%u,rtt_ms=%" PRIu32
                 ",ttl=%u,size=%" PRIu32,
                 probe->name,
                 uptime_ms(),
                 probe->dst,
                 (unsigned)seq,
                 rtt_ms,
                 (unsigned)ttl,
                 size);
    }
    maybe_print_summary(probe);
}

static void record_timeout(struct ping_probe *probe, uint16_t seq)
{
    probe->sent++;

    if (probe->verbose)
    {
        lab_outf("PING,%s,t_ms=%" PRIu32 ",ev=timeout,dst=%s,seq=%u",
                 probe->name,
                 uptime_ms(),
                 probe->dst,
                 (unsigned)seq);
    }
    maybe_print_summary(probe);
}

/* Sleep until @p until_us (esp_timer time), waking early on a stop request. */
static void sleep_until(struct ping_probe *probe, int64_t until_us)
{
    while (!probe->stop)
    {
        int64_t left_ms = (until_us - esp_timer_get_time()) / 1000;
        if (left_ms <= 0)
        {
            return;
        }
        lab_sleep_ms((uint32_t)((left_ms < PING_POLL_MS) ? left_ms : PING_POLL_MS));
    }
}

/*
 * Wait for the reply to (@p id, @p seq) until @p deadline_us. Returns true and fills @p ttl and
 * @p size (ICMP data bytes, as esp_ping reports them) if it arrived in time.
 */
static bool wait_reply(struct ping_probe *probe, int sock, uint16_t id, uint16_t seq,
                       int64_t deadline_us, uint8_t *ttl, uint32_t *size)
{
    static const size_t hdrs = sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr);
    uint8_t buf[64];

    while (!probe->stop)
    {
        int64_t left_us = deadline_us - esp_timer_get_time();
        if (left_us <= 0)
        {
            return false;
        }
        if (left_us > PING_POLL_MS * 1000)
        {
            left_us = PING_POLL_MS * 1000;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = (suseconds_t)left_us,
        };
        if (select(sock + 1, &rfds, NULL, NULL, &tv) <= 0)
        {
            continue;
        }

        /* Only the headers are needed; the rest of a datagram is discarded by recv(). */
        int len = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (len < (int)hdrs)
        {
            continue;
        }
        const struct ip_hdr *iphdr = (const struct ip_hdr *)buf;
        size_t hl = IPH_HL_BYTES(iphdr);
        if ((size_t)len < hl + sizeof(struct icmp_echo_hdr))
        {
            continue;
        }
        const struct icmp_echo_hdr *echo = (const struct icmp_echo_hdr *)(buf + hl);
        if (ICMPH_TYPE(echo) == ICMP_ER && echo->id == lwip_htons(id) &&
            echo->seqno == lwip_htons(seq))
        {
            *ttl = IPH_TTL(iphdr);
            uint16_t ip_len = lwip_ntohs(IPH_LEN(iphdr));
            *size = (ip_len > hl + sizeof(struct icmp_echo_hdr)) ?
                        (uint32_t)(ip_len - hl - sizeof(struct icmp_echo_hdr)) : 0;
            return true;
        }
    }
    return false;
}

static void ping_task(void *arg)
{
    struct ping_probe *probe = (struct ping_probe *)arg;
    size_t data_size = (probe->data_size > PING_MAX_DATA) ? PING_MAX_DATA : probe->data_size;
    size_t pkt_len = sizeof(struct icmp_echo_hdr) + data_size;
    uint8_t *pkt = malloc(pkt_len);
    int sock = -1;

    struct sockaddr_in to = { .sin_family = AF_INET };
    if (pkt == NULL || inet_aton(probe->dst, &to.sin_addr) == 0)
    {
        ESP_LOGE(TAG, "%s: %s", probe->name, (pkt == NULL) ? "out of memory" : "bad address");
        goto out;
    }

    sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0 || connect(sock, (struct sockaddr *)&to, sizeof(to)) != 0)
    {
        ESP_LOGE(TAG, "%s: ICMP socket setup failed, errno %d", probe->name, errno);
        goto out;
    }
    int ttl_opt = PING_TTL;
    (void)setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl_opt, sizeof(ttl_opt));

    struct icmp_echo_hdr *echo = (struct icmp_echo_hdr *)pkt;
    for (size_t ii = 0; ii < data_size; ii++)
    {
        pkt[sizeof(*echo) + ii] = (uint8_t)('A' + (ii % 26));
    }

    probe->next_summary_ms = uptime_ms() + probe->report_interval_ms;
    uint16_t seq = 0;
    int64_t next_send_us = esp_timer_get_time();

    while (!probe->stop)
    {
        if (++seq == 0)
        {
            seq = 1;
        }
        ICMPH_TYPE_SET(echo, ICMP_ECHO);
        ICMPH_CODE_SET(echo, 0);
        echo->id = lwip_htons(probe->echo_id);
        echo->seqno = lwip_htons(seq);
        echo->chksum = 0;
        echo->chksum = inet_chksum(pkt, (u16_t)pkt_len);

        int64_t sent_us = esp_timer_get_time();
        uint8_t ttl = 0;
        uint32_t size = 0;
        bool replied = false;
        if (send(sock, pkt, pkt_len, 0) == (ssize_t)pkt_len)
        {
            replied = wait_reply(probe, sock, probe->echo_id, seq,
                                 sent_us + (int64_t)probe->timeout_ms * 1000, &ttl, &size);
        }
        else
        {
            /* No route or no buffer: counted as a timeout once the timeout has passed. */
            sleep_until(probe, sent_us + (int64_t)probe->timeout_ms * 1000);
        }
        if (probe->stop)
        {
            break;
        }

        if (replied)
        {
            record_reply(probe, seq, (uint32_t)((esp_timer_get_time() - sent_us) / 1000), ttl,
                         size);
        }
        else
        {
            record_timeout(probe, seq);
        }

        /* Fixed send schedule, but no burst of catch-up pings after a long wait. */
        next_send_us += (int64_t)probe->interval_ms * 1000;
        int64_t now = esp_timer_get_time();
        if (next_send_us < now)
        {
            next_send_us = now;
        }
        sleep_until(probe, next_send_us);
    }

    print_summary(probe);

out:
    if (sock >= 0)
    {
        close(sock);
    }
    free(pkt);
    probe->active = false;
    vTaskDelete(NULL);
}

esp_err_t ping_probe_start(struct ping_probe *probe)
{
    if (probe->active)
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* A new id per run, so a late reply to the previous run is never taken for this one. */
    probe->echo_id = next_echo_id++;

    probe->stop = false;
    probe->active = true;
    if (xTaskCreate(ping_task, probe->name, PING_TASK_STACK_BYTES, probe, PING_TASK_PRIO, NULL) !=
        pdPASS)
    {
        probe->active = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ping_probe_stop(struct ping_probe *probe)
{
    probe->stop = true;
}

bool ping_probe_is_active(const struct ping_probe *probe)
{
    return probe->active;
}
