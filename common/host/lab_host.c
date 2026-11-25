/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host (POSIX) front end for the lab traffic services, used by the netns simulation and the
 * loopback integration test. Prints the same STATS/EVENT lines as the firmware.
 *
 *   lab_host udp-server [--port N] [--report-ms N]
 *   lab_host udp-client [--dst IP] [--port N] [--rate N] [--size N] [--count N]
 *                       [--duration-ms N] [--report-ms N] [--drain-ms N] [--max-lost N]
 *   lab_host tcp-server [--port N] [--report-ms N] [--idle-ms N]
 *   lab_host tcp-client [--dst IP] [--port N] [--rate N] [--size N] [--count N]
 *                       [--duration-ms N] [--report-ms N] [--ack-timeout-ms N]
 *                       [--connect-timeout-ms N] [--backoff-min-ms N] [--backoff-max-ms N]
 *                       [--inflight N] [--max-lost N]
 *   Common: [--flow NAME]
 *
 * Exit status: 0 ok, 1 usage/setup error, 2 client finished with more than --max-lost lost.
 */

#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lab_platform.h"
#include "lab_services.h"

static volatile bool stop_flag;

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = true;
}

struct opts
{
    const char *dst;
    const char *flow;
    long port;
    long rate;
    long size;
    long count;
    long duration_ms;
    long report_ms;
    long drain_ms;
    long idle_ms;
    long ack_timeout_ms;
    long connect_timeout_ms;
    long backoff_min_ms;
    long backoff_max_ms;
    long inflight;
    long max_lost;
};

static void usage(void)
{
    fprintf(stderr,
            "usage: lab_host {udp-server|udp-client|tcp-server|tcp-client} [options]\n"
            "  --dst IP --port N --rate N --size N --count N --duration-ms N --report-ms N\n"
            "  --drain-ms N --idle-ms N --ack-timeout-ms N --connect-timeout-ms N\n"
            "  --backoff-min-ms N --backoff-max-ms N --inflight N --max-lost N --flow NAME\n");
}

static int parse_long(const char *s, long *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || v < 0)
    {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
    static const struct
    {
        const char *name;
        size_t off;
    } num_opts[] = {
        { "--port", offsetof(struct opts, port) },
        { "--rate", offsetof(struct opts, rate) },
        { "--size", offsetof(struct opts, size) },
        { "--count", offsetof(struct opts, count) },
        { "--duration-ms", offsetof(struct opts, duration_ms) },
        { "--report-ms", offsetof(struct opts, report_ms) },
        { "--drain-ms", offsetof(struct opts, drain_ms) },
        { "--idle-ms", offsetof(struct opts, idle_ms) },
        { "--ack-timeout-ms", offsetof(struct opts, ack_timeout_ms) },
        { "--connect-timeout-ms", offsetof(struct opts, connect_timeout_ms) },
        { "--backoff-min-ms", offsetof(struct opts, backoff_min_ms) },
        { "--backoff-max-ms", offsetof(struct opts, backoff_max_ms) },
        { "--inflight", offsetof(struct opts, inflight) },
        { "--max-lost", offsetof(struct opts, max_lost) },
    };

    for (int i = 2; i < argc; i++)
    {
        if (i + 1 >= argc)
        {
            fprintf(stderr, "missing value for %s\n", argv[i]);
            return -1;
        }
        const char *name = argv[i];
        const char *val = argv[++i];
        if (strcmp(name, "--dst") == 0)
        {
            o->dst = val;
            continue;
        }
        if (strcmp(name, "--flow") == 0)
        {
            o->flow = val;
            continue;
        }

        bool found = false;
        for (size_t k = 0; k < sizeof(num_opts) / sizeof(num_opts[0]); k++)
        {
            if (strcmp(name, num_opts[k].name) == 0)
            {
                if (parse_long(val, (long *)((char *)o + num_opts[k].off)) != 0)
                {
                    fprintf(stderr, "bad value for %s: %s\n", name, val);
                    return -1;
                }
                found = true;
                break;
            }
        }
        if (!found)
        {
            fprintf(stderr, "unknown option %s\n", name);
            return -1;
        }
    }
    return 0;
}

#define SET_IF(field, value, type) \
    do                             \
    {                              \
        if ((value) >= 0)          \
        {                          \
            (field) = (type)(value); \
        }                          \
    } while (0)

int main(int argc, char **argv)
{
    struct opts o;
    memset(&o, 0, sizeof(o));
    o.port = o.rate = o.size = o.count = o.duration_ms = o.report_ms = o.drain_ms = -1;
    o.idle_ms = o.ack_timeout_ms = o.connect_timeout_ms = -1;
    o.backoff_min_ms = o.backoff_max_ms = o.inflight = o.max_lost = -1;

    if (argc < 2 || parse_opts(argc, argv, &o) != 0)
    {
        usage();
        return 1;
    }

    lab_platform_init();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const char *mode = argv[1];
    struct linkstats st;
    int rc;

    if (strcmp(mode, "udp-server") == 0)
    {
        struct udp_echo_server_cfg cfg;
        udp_echo_server_cfg_init(&cfg);
        SET_IF(cfg.port, o.port, uint16_t);
        SET_IF(cfg.report_interval_ms, o.report_ms, uint32_t);
        cfg.flow = o.flow != NULL ? o.flow : cfg.flow;
        cfg.stop = &stop_flag;
        rc = udp_echo_server_run(&cfg);
        return rc == LAB_SVC_OK ? 0 : 1;
    }
    if (strcmp(mode, "tcp-server") == 0)
    {
        struct tcp_stream_server_cfg cfg;
        tcp_stream_server_cfg_init(&cfg);
        SET_IF(cfg.port, o.port, uint16_t);
        SET_IF(cfg.report_interval_ms, o.report_ms, uint32_t);
        SET_IF(cfg.idle_timeout_ms, o.idle_ms, uint32_t);
        cfg.flow = o.flow != NULL ? o.flow : cfg.flow;
        cfg.stop = &stop_flag;
        rc = tcp_stream_server_run(&cfg);
        return rc == LAB_SVC_OK ? 0 : 1;
    }
    if (strcmp(mode, "udp-client") == 0)
    {
        struct udp_echo_client_cfg cfg;
        udp_echo_client_cfg_init(&cfg);
        cfg.dst_ip = o.dst != NULL ? o.dst : cfg.dst_ip;
        SET_IF(cfg.dst_port, o.port, uint16_t);
        SET_IF(cfg.rate_pps, o.rate, uint32_t);
        SET_IF(cfg.payload_len, o.size, uint16_t);
        SET_IF(cfg.count, o.count, uint32_t);
        SET_IF(cfg.duration_ms, o.duration_ms, uint32_t);
        SET_IF(cfg.report_interval_ms, o.report_ms, uint32_t);
        SET_IF(cfg.drain_ms, o.drain_ms, uint32_t);
        cfg.flow = o.flow != NULL ? o.flow : cfg.flow;
        cfg.stop = &stop_flag;
        rc = udp_echo_client_run(&cfg, &st);
    }
    else if (strcmp(mode, "tcp-client") == 0)
    {
        struct tcp_stream_client_cfg cfg;
        tcp_stream_client_cfg_init(&cfg);
        cfg.dst_ip = o.dst != NULL ? o.dst : cfg.dst_ip;
        SET_IF(cfg.dst_port, o.port, uint16_t);
        SET_IF(cfg.rate_mps, o.rate, uint32_t);
        SET_IF(cfg.payload_len, o.size, uint16_t);
        SET_IF(cfg.count, o.count, uint32_t);
        SET_IF(cfg.duration_ms, o.duration_ms, uint32_t);
        SET_IF(cfg.report_interval_ms, o.report_ms, uint32_t);
        SET_IF(cfg.ack_timeout_ms, o.ack_timeout_ms, uint32_t);
        SET_IF(cfg.connect_timeout_ms, o.connect_timeout_ms, uint32_t);
        SET_IF(cfg.backoff_min_ms, o.backoff_min_ms, uint32_t);
        SET_IF(cfg.backoff_max_ms, o.backoff_max_ms, uint32_t);
        SET_IF(cfg.max_inflight, o.inflight, uint32_t);
        cfg.flow = o.flow != NULL ? o.flow : cfg.flow;
        cfg.stop = &stop_flag;
        rc = tcp_stream_client_run(&cfg, &st);
    }
    else
    {
        usage();
        return 1;
    }

    if (rc != LAB_SVC_OK)
    {
        fprintf(stderr, "service failed: %d\n", rc);
        return 1;
    }
    if (o.max_lost >= 0 && st.lost > (uint32_t)o.max_lost)
    {
        return 2;
    }
    return 0;
}
