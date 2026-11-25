/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Node B (HaLow relay root AP) on the host: UDP echo server and TCP stream server from common/,
 * each in its own task as on the ESP32-S3. Node B only answers traffic and never starts any.
 * Runs until SIGINT/SIGTERM (or --duration-ms), then prints the final per-session reports.
 *
 * Options mirror the CONFIG_LAB_* Kconfig names (lower case, '-' for '_').
 */

#include "halow_lab_config.h"
#include "host_app_util.h"
#include "lab_platform.h"
#include "lab_services.h"

static struct
{
    uint32_t udp_echo_port;
    uint32_t tcp_stream_port;
    uint32_t report_interval_ms;
    uint32_t tcp_idle_timeout_ms;
    uint32_t duration_ms;
    bool udp;
    bool tcp;
    const char *udp_flow;
    const char *tcp_flow;
} cfg = {
    .udp_echo_port = LAB_UDP_ECHO_PORT,
    .tcp_stream_port = LAB_TCP_STREAM_PORT,
    .report_interval_ms = LAB_REPORT_INTERVAL_MS,
    .tcp_idle_timeout_ms = LAB_TCP_SERVER_IDLE_TIMEOUT_MS,
    .udp = true,
    .tcp = true,
    .udp_flow = "udp_srv",
    .tcp_flow = "tcp_srv",
};

static const struct host_opt opts[] = {
    { "udp-echo-port", HOST_OPT_U32, &cfg.udp_echo_port, "UDP echo port" },
    { "tcp-stream-port", HOST_OPT_U32, &cfg.tcp_stream_port, "TCP stream port" },
    { "report-interval-ms", HOST_OPT_U32, &cfg.report_interval_ms, "STATS period, 0 = final" },
    { "tcp-idle-timeout-ms", HOST_OPT_U32, &cfg.tcp_idle_timeout_ms, "0 = never" },
    { "duration-ms", HOST_OPT_U32, &cfg.duration_ms, "run time, 0 = until SIGINT/SIGTERM" },
    { "udp", HOST_OPT_BOOL, &cfg.udp, "run the UDP echo server" },
    { "tcp", HOST_OPT_BOOL, &cfg.tcp, "run the TCP stream server" },
    { "udp-flow", HOST_OPT_STR, &cfg.udp_flow, "flow name of the UDP server" },
    { "tcp-flow", HOST_OPT_STR, &cfg.tcp_flow, "flow name of the TCP server" },
};

static struct udp_echo_server_cfg udp_cfg;
static struct tcp_stream_server_cfg tcp_cfg;

static void udp_task(void *arg)
{
    (void)arg;
    int rc = udp_echo_server_run(&udp_cfg);
    if (rc != LAB_SVC_OK)
    {
        lab_log(LAB_LOG_ERR, "udp_echo_server_run failed: %d", rc);
    }
}

static void tcp_task(void *arg)
{
    (void)arg;
    int rc = tcp_stream_server_run(&tcp_cfg);
    if (rc != LAB_SVC_OK)
    {
        lab_log(LAB_LOG_ERR, "tcp_stream_server_run failed: %d", rc);
    }
}

int main(int argc, char **argv)
{
    if (host_app_parse(argc, argv, "node_b_host", opts, sizeof(opts) / sizeof(opts[0])) != 0)
    {
        return 1;
    }
    host_app_init();
    uint64_t start_us = lab_time_us();

    lab_outf("EVENT,node_b,t_ms=0,ev=boot,udp_port=%lu,tcp_port=%lu",
             (unsigned long)cfg.udp_echo_port, (unsigned long)cfg.tcp_stream_port);

    if (cfg.udp)
    {
        udp_echo_server_cfg_init(&udp_cfg);
        udp_cfg.port = (uint16_t)cfg.udp_echo_port;
        udp_cfg.report_interval_ms = cfg.report_interval_ms;
        udp_cfg.flow = cfg.udp_flow;
        udp_cfg.stop = &host_app_stop;
        if (host_app_spawn(udp_task, NULL, "udp_srv") != 0)
        {
            return 1;
        }
    }
    if (cfg.tcp)
    {
        tcp_stream_server_cfg_init(&tcp_cfg);
        tcp_cfg.port = (uint16_t)cfg.tcp_stream_port;
        tcp_cfg.report_interval_ms = cfg.report_interval_ms;
        tcp_cfg.idle_timeout_ms = cfg.tcp_idle_timeout_ms;
        tcp_cfg.flow = cfg.tcp_flow;
        tcp_cfg.stop = &host_app_stop;
        if (host_app_spawn(tcp_task, NULL, "tcp_srv") != 0)
        {
            return 1;
        }
    }

    host_app_wait(cfg.duration_ms);
    lab_outf("EVENT,node_b,t_ms=%lu,ev=exit",
             (unsigned long)((lab_time_us() - start_us) / 1000U));
    return 0;
}
