/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "halow_lab_config.h"
#include "lab_platform.h"
#include "lab_services.h"
#include "tput.h"

static const char *TAG = "tput";

#define TPUT_MAGIC         0x54505554u /* "TPUT" */
#define TPUT_FLAG_END      1u
#define TPUT_HDR_LEN       16
#define TPUT_UDP_IDLE_MS   2000
#define TPUT_TCP_BUF_LEN   4096
#define TPUT_TASK_STACK    4096
/* The UDP sender sleeps one tick after this many datagrams so lower-priority tasks still run. */
#define TPUT_UDP_BURST     8

static struct tput_server_cfg server_cfg;

static uint32_t uptime_ms(void)
{
    return (uint32_t)(lab_time_us() / 1000);
}

static uint32_t kbps(uint64_t bytes, uint64_t us)
{
    return (us == 0) ? 0 : (uint32_t)((bytes * 8u * 1000u) / us);
}

/* ---- TCP sink --------------------------------------------------------------------------- */

static void tcp_sink_conn(int conn, uint32_t test, uint8_t *buf)
{
    struct timeval tv = { .tv_sec = 1 };
    (void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint64_t total = 0;
    uint64_t first_us = 0;
    uint64_t last_us = 0;
    uint64_t interval_bytes = 0;
    uint64_t interval_start_us = lab_time_us();

    for (;;)
    {
        int len = recv(conn, buf, TPUT_TCP_BUF_LEN, 0);
        uint64_t now = lab_time_us();
        if (len > 0)
        {
            if (total == 0)
            {
                first_us = now;
                interval_start_us = now;
            }
            total += (uint64_t)len;
            interval_bytes += (uint64_t)len;
            last_us = now;
        }
        else if (len == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            break;
        }

        uint64_t elapsed = now - interval_start_us;
        if (total > 0 && elapsed >= (uint64_t)server_cfg.report_interval_ms * 1000u)
        {
            lab_outf("STATS,tput_srv,t_ms=%" PRIu32 ",proto=tcp,test=%" PRIu32
                     ",interval_ms=%" PRIu32 ",bytes=%" PRIu64 ",kbps=%" PRIu32,
                     uptime_ms(), test, (uint32_t)(elapsed / 1000), interval_bytes,
                     kbps(interval_bytes, elapsed));
            interval_bytes = 0;
            interval_start_us = now;
        }
    }

    lab_outf("EVENT,tput_srv,t_ms=%" PRIu32 ",ev=end,proto=tcp,test=%" PRIu32 ",ms=%" PRIu32
             ",bytes=%" PRIu64 ",kbps=%" PRIu32,
             uptime_ms(), test, (uint32_t)((last_us - first_us) / 1000), total,
             kbps(total, last_us - first_us));
}

static void tcp_sink_task(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(TPUT_TCP_BUF_LEN);
    uint32_t test = 0;

    for (;;)
    {
        int lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(server_cfg.tcp_port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        int one = 1;
        if (buf == NULL || lsock < 0)
        {
            ESP_LOGE(TAG, "TCP sink setup failed");
            vTaskDelete(NULL);
        }
        (void)setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(lsock, 1) != 0)
        {
            ESP_LOGE(TAG, "TCP sink bind/listen failed, errno %d", errno);
            close(lsock);
            lab_sleep_ms(2000);
            continue;
        }
        lab_outf("EVENT,tput_srv,t_ms=%" PRIu32 ",ev=listening,proto=tcp,port=%u", uptime_ms(),
                 (unsigned)server_cfg.tcp_port);

        for (;;)
        {
            int conn = accept(lsock, NULL, NULL);
            if (conn < 0)
            {
                break;
            }
            tcp_sink_conn(conn, ++test, buf);
            close(conn);
        }
        close(lsock);
    }
}

/* ---- UDP sink --------------------------------------------------------------------------- */

struct udp_test
{
    bool active;
    uint32_t id;
    uint64_t first_us;
    uint64_t last_us;
    uint64_t bytes;
    uint32_t pkts;
    uint32_t max_seq;
    uint64_t interval_start_us;
    uint64_t interval_bytes;
    uint32_t interval_pkts;
};

static void udp_test_end(struct udp_test *t)
{
    if (!t->active)
    {
        return;
    }
    uint32_t expected = t->max_seq + 1;
    uint32_t lost = (expected > t->pkts) ? expected - t->pkts : 0;
    uint32_t loss_pct_x100 = (expected > 0) ? (uint32_t)(((uint64_t)lost * 10000u) / expected) : 0;

    lab_outf("EVENT,tput_srv,t_ms=%" PRIu32 ",ev=end,proto=udp,test=%" PRIu32 ",ms=%" PRIu32
             ",bytes=%" PRIu64 ",kbps=%" PRIu32 ",pkts=%" PRIu32 ",lost=%" PRIu32
             ",loss_pct=%" PRIu32 ".%02" PRIu32,
             uptime_ms(), t->id, (uint32_t)((t->last_us - t->first_us) / 1000), t->bytes,
             kbps(t->bytes, t->last_us - t->first_us), t->pkts, lost, loss_pct_x100 / 100,
             loss_pct_x100 % 100);
    t->active = false;
}

static void udp_sink_task(void *arg)
{
    (void)arg;
    static uint8_t buf[TPUT_UDP_MAX_LEN + 64];
    struct udp_test t = { 0 };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(server_cfg.udp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGE(TAG, "UDP sink setup failed, errno %d", errno);
        vTaskDelete(NULL);
    }
    struct timeval tv = { .tv_usec = 200000 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int rcvbuf = 32 * 1024;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    lab_outf("EVENT,tput_srv,t_ms=%" PRIu32 ",ev=listening,proto=udp,port=%u", uptime_ms(),
             (unsigned)server_cfg.udp_port);

    for (;;)
    {
        int len = recv(sock, buf, sizeof(buf), 0);
        uint64_t now = lab_time_us();

        if (len >= TPUT_HDR_LEN)
        {
            uint32_t hdr[4];
            memcpy(hdr, buf, sizeof(hdr));
            uint32_t magic = ntohl(hdr[0]);
            uint32_t id = ntohl(hdr[1]);
            uint32_t seq = ntohl(hdr[2]);
            uint32_t flags = ntohl(hdr[3]);

            if (magic == TPUT_MAGIC)
            {
                if (flags & TPUT_FLAG_END)
                {
                    if (t.active && t.id == id)
                    {
                        udp_test_end(&t);
                    }
                }
                else
                {
                    if (!t.active || t.id != id)
                    {
                        udp_test_end(&t);
                        memset(&t, 0, sizeof(t));
                        t.active = true;
                        t.id = id;
                        t.first_us = now;
                        t.interval_start_us = now;
                    }
                    t.bytes += (uint64_t)len;
                    t.pkts++;
                    t.last_us = now;
                    t.interval_bytes += (uint64_t)len;
                    t.interval_pkts++;
                    if (seq > t.max_seq)
                    {
                        t.max_seq = seq;
                    }
                }
            }
        }

        if (t.active)
        {
            uint64_t elapsed = now - t.interval_start_us;
            if (elapsed >= (uint64_t)server_cfg.report_interval_ms * 1000u)
            {
                lab_outf("STATS,tput_srv,t_ms=%" PRIu32 ",proto=udp,test=%" PRIu32
                         ",interval_ms=%" PRIu32 ",bytes=%" PRIu64 ",kbps=%" PRIu32
                         ",pkts=%" PRIu32,
                         uptime_ms(), t.id, (uint32_t)(elapsed / 1000), t.interval_bytes,
                         kbps(t.interval_bytes, elapsed), t.interval_pkts);
                t.interval_bytes = 0;
                t.interval_pkts = 0;
                t.interval_start_us = now;
            }
            if (now - t.last_us > (uint64_t)TPUT_UDP_IDLE_MS * 1000u)
            {
                udp_test_end(&t);
            }
        }
    }
}

int tput_server_start(const struct tput_server_cfg *cfg)
{
    server_cfg = *cfg;
    if (server_cfg.report_interval_ms == 0)
    {
        server_cfg.report_interval_ms = 1000;
    }
    if (lab_task_create(tcp_sink_task, NULL, "tput_tcp", TPUT_TASK_STACK, LAB_TASK_PRIO_NORMAL) !=
            0 ||
        lab_task_create(udp_sink_task, NULL, "tput_udp", TPUT_TASK_STACK, LAB_TASK_PRIO_NORMAL) !=
            0)
    {
        return -1;
    }
    return 0;
}

/* ---- Client ----------------------------------------------------------------------------- */

static bool stop_requested(const struct tput_client_cfg *cfg)
{
    return cfg->stop != NULL && *cfg->stop;
}

int32_t tput_client_run(const struct tput_client_cfg *cfg)
{
    bool udp = (cfg->proto == TPUT_UDP);
    uint32_t len = cfg->len;
    if (udp)
    {
        len = (len < TPUT_HDR_LEN) ? TPUT_HDR_LEN : len;
        len = (len > TPUT_UDP_MAX_LEN) ? TPUT_UDP_MAX_LEN : len;
    }
    else if (len == 0)
    {
        len = 1460;
    }

    uint8_t *buf = malloc(len);
    int sock = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, udp ? IPPROTO_UDP : IPPROTO_TCP);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(cfg->port) };
    uint32_t test = lab_random_u32();
    int32_t result = -1;

    if (buf == NULL || sock < 0 || inet_aton(cfg->dst_ip, &to.sin_addr) == 0)
    {
        ESP_LOGE(TAG, "client setup failed");
        goto out;
    }
    for (uint32_t ii = 0; ii < len; ii++)
    {
        buf[ii] = (uint8_t)ii;
    }
    struct timeval tv = { .tv_sec = 1 };
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&to, sizeof(to)) != 0)
    {
        lab_outf("EVENT,tput,t_ms=%" PRIu32 ",ev=connect_failed,proto=%s,dst=%s,err=%d",
                 uptime_ms(), udp ? "udp" : "tcp", cfg->dst_ip, errno);
        goto out;
    }

    lab_outf("EVENT,tput,t_ms=%" PRIu32 ",ev=start,proto=%s,test=%" PRIu32
             ",dst=%s:%u,duration_ms=%" PRIu32 ",len=%" PRIu32,
             uptime_ms(), udp ? "udp" : "tcp", test, cfg->dst_ip, (unsigned)cfg->port,
             cfg->duration_ms, len);

    uint64_t start = lab_time_us();
    uint64_t end = start + (uint64_t)cfg->duration_ms * 1000u;
    uint64_t bytes = 0;
    uint32_t pkts = 0;
    uint32_t send_err = 0;
    uint32_t hdr[4] = { htonl(TPUT_MAGIC), htonl(test), 0, 0 };

    while (lab_time_us() < end && !stop_requested(cfg))
    {
        if (udp)
        {
            hdr[2] = htonl(pkts);
            memcpy(buf, hdr, sizeof(hdr));
        }
        int n = send(sock, buf, len, 0);
        if (n > 0)
        {
            bytes += (uint64_t)n;
            pkts++;
            if (udp && (pkts % TPUT_UDP_BURST) == 0)
            {
                vTaskDelay(1);
            }
        }
        else if (udp || errno == EAGAIN || errno == EWOULDBLOCK)
        {
            /* UDP: lwIP or the driver is out of buffers. TCP: send timeout, window full. */
            send_err++;
            vTaskDelay(1);
        }
        else
        {
            lab_outf("EVENT,tput,t_ms=%" PRIu32 ",ev=send_failed,proto=tcp,err=%d", uptime_ms(),
                     errno);
            break;
        }
    }
    uint64_t elapsed = lab_time_us() - start;

    if (udp)
    {
        hdr[3] = htonl(TPUT_FLAG_END);
        memcpy(buf, hdr, sizeof(hdr));
        for (int ii = 0; ii < 3; ii++)
        {
            lab_sleep_ms(50);
            (void)send(sock, buf, TPUT_HDR_LEN, 0);
        }
    }

    result = (int32_t)kbps(bytes, elapsed);
    lab_outf("EVENT,tput,t_ms=%" PRIu32 ",ev=end,proto=%s,test=%" PRIu32 ",ms=%" PRIu32
             ",bytes=%" PRIu64 ",kbps=%" PRId32 ",pkts=%" PRIu32 ",send_err=%" PRIu32,
             uptime_ms(), udp ? "udp" : "tcp", test, (uint32_t)(elapsed / 1000), bytes, result,
             pkts, send_err);

out:
    if (sock >= 0)
    {
        close(sock);
    }
    free(buf);
    return result;
}

void tput_client_loop(const char *dst_ip, uint32_t duration_ms, uint32_t pause_ms, uint32_t udp_len,
                      const volatile bool *stop)
{
    static const enum tput_proto protos[] = { TPUT_TCP, TPUT_UDP };

    for (unsigned round = 0; stop == NULL || !*stop; round++)
    {
        enum tput_proto proto = protos[round % 2];
        struct tput_client_cfg cfg = {
            .proto = proto,
            .dst_ip = dst_ip,
            .port = (proto == TPUT_TCP) ? LAB_TPUT_TCP_PORT : LAB_TPUT_UDP_PORT,
            .duration_ms = duration_ms,
            .len = (proto == TPUT_TCP) ? 1460 : udp_len,
            .stop = stop,
        };
        (void)tput_client_run(&cfg);
        for (uint32_t waited = 0; waited < pause_ms && (stop == NULL || !*stop);
             waited += LAB_SVC_POLL_MS)
        {
            lab_sleep_ms(LAB_SVC_POLL_MS);
        }
    }
}
