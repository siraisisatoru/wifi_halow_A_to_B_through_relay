/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Relay (EKH05 repeater) on the host: the relay's L2 forwarding is the netns bridge, so this
 * app only provides the relay's own UDP echo server. Node A uses it to measure the A<->relay
 * hop on its own, which the firmware can offer the same way with common/ on mmosal.
 */

#include "halow_lab_config.h"
#include "host_app_util.h"
#include "lab_platform.h"
#include "lab_services.h"

static struct
{
    uint32_t udp_echo_port;
    uint32_t report_interval_ms;
    uint32_t duration_ms;
    const char *udp_flow;
} cfg = {
    .udp_echo_port = LAB_UDP_ECHO_PORT,
    .report_interval_ms = LAB_REPORT_INTERVAL_MS,
    .udp_flow = "udp_relay_srv",
};

static const struct host_opt opts[] = {
    { "udp-echo-port", HOST_OPT_U32, &cfg.udp_echo_port, "UDP echo port" },
    { "report-interval-ms", HOST_OPT_U32, &cfg.report_interval_ms, "STATS period, 0 = final" },
    { "duration-ms", HOST_OPT_U32, &cfg.duration_ms, "run time, 0 = until SIGINT/SIGTERM" },
    { "udp-flow", HOST_OPT_STR, &cfg.udp_flow, "flow name of the UDP server" },
};

static struct udp_echo_server_cfg udp_cfg;

static void udp_task(void *arg)
{
    (void)arg;
    int rc = udp_echo_server_run(&udp_cfg);
    if (rc != LAB_SVC_OK)
    {
        lab_log(LAB_LOG_ERR, "udp_echo_server_run failed: %d", rc);
    }
}

int main(int argc, char **argv)
{
    if (host_app_parse(argc, argv, "relay_host", opts, sizeof(opts) / sizeof(opts[0])) != 0)
    {
        return 1;
    }
    host_app_init();
    uint64_t start_us = lab_time_us();

    lab_outf("EVENT,relay,t_ms=0,ev=boot,udp_port=%lu", (unsigned long)cfg.udp_echo_port);

    udp_echo_server_cfg_init(&udp_cfg);
    udp_cfg.port = (uint16_t)cfg.udp_echo_port;
    udp_cfg.report_interval_ms = cfg.report_interval_ms;
    udp_cfg.flow = cfg.udp_flow;
    udp_cfg.stop = &host_app_stop;
    if (host_app_spawn(udp_task, NULL, "udp_srv") != 0)
    {
        return 1;
    }

    host_app_wait(cfg.duration_ms);
    lab_outf("EVENT,relay,t_ms=%lu,ev=exit",
             (unsigned long)((lab_time_us() - start_us) / 1000U));
    return 0;
}
