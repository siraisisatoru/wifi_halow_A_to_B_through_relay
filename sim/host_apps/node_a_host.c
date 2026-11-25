/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Node A (HaLow STA behind the relay) on the host: runs the same common/ services as the
 * ESP32-S3 firmware. UDP echo client and TCP stream client towards node B, optionally a second
 * UDP echo client towards the relay. ICMP ping is done by iputils in the simulation, as esp_ping
 * does it on the board.
 *
 * Options mirror the CONFIG_LAB_* Kconfig names (lower case, '-' for '_').
 */

#include <stdio.h>

#include "halow_lab_config.h"
#include "host_app_util.h"
#include "lab_platform.h"
#include "lab_services.h"

static struct
{
    const char *ip_node_b;
    const char *ip_relay;
    uint32_t udp_echo_port;
    uint32_t tcp_stream_port;
    uint32_t udp_rate_pps;
    uint32_t udp_payload_len;
    uint32_t udp_count;
    uint32_t udp_drain_ms;
    uint32_t tcp_rate_mps;
    uint32_t tcp_payload_len;
    uint32_t tcp_count;
    uint32_t tcp_ack_timeout_ms;
    uint32_t tcp_connect_timeout_ms;
    uint32_t tcp_backoff_min_ms;
    uint32_t tcp_backoff_max_ms;
    uint32_t tcp_max_inflight;
    uint32_t relay_echo_rate_pps;
    uint32_t report_interval_ms;
    uint32_t duration_ms;
    bool udp;
    bool tcp;
    bool relay_echo;
    const char *udp_flow;
    const char *tcp_flow;
    const char *relay_flow;
} cfg = {
    .ip_node_b = LAB_IP_NODE_B,
    .ip_relay = LAB_IP_RELAY,
    .udp_echo_port = LAB_UDP_ECHO_PORT,
    .tcp_stream_port = LAB_TCP_STREAM_PORT,
    .udp_rate_pps = LAB_UDP_RATE_PPS,
    .udp_payload_len = LAB_UDP_PAYLOAD_LEN,
    .udp_drain_ms = LAB_UDP_DRAIN_MS,
    .tcp_rate_mps = LAB_TCP_RATE_MPS,
    .tcp_payload_len = LAB_TCP_PAYLOAD_LEN,
    .tcp_ack_timeout_ms = LAB_TCP_ACK_TIMEOUT_MS,
    .tcp_connect_timeout_ms = LAB_TCP_CONNECT_TIMEOUT_MS,
    .tcp_backoff_min_ms = LAB_TCP_BACKOFF_MIN_MS,
    .tcp_backoff_max_ms = LAB_TCP_BACKOFF_MAX_MS,
    .tcp_max_inflight = LAB_TCP_MAX_INFLIGHT,
    .relay_echo_rate_pps = LAB_UDP_RATE_PPS,
    .report_interval_ms = LAB_REPORT_INTERVAL_MS,
    .udp = true,
    .tcp = true,
    .udp_flow = "udp",
    .tcp_flow = "tcp",
    .relay_flow = "udp_relay",
};

static const struct host_opt opts[] = {
    { "ip-node-b", HOST_OPT_STR, &cfg.ip_node_b, "node B (root AP) address" },
    { "ip-relay", HOST_OPT_STR, &cfg.ip_relay, "relay address (for --relay-echo)" },
    { "udp-echo-port", HOST_OPT_U32, &cfg.udp_echo_port, "UDP echo port" },
    { "tcp-stream-port", HOST_OPT_U32, &cfg.tcp_stream_port, "TCP stream port" },
    { "udp-rate-pps", HOST_OPT_U32, &cfg.udp_rate_pps, "UDP echo requests per second" },
    { "udp-payload-len", HOST_OPT_U32, &cfg.udp_payload_len, "UDP payload bytes (<=1400)" },
    { "udp-count", HOST_OPT_U32, &cfg.udp_count, "UDP requests to send, 0 = unlimited" },
    { "udp-drain-ms", HOST_OPT_U32, &cfg.udp_drain_ms, "wait for late echoes at the end" },
    { "tcp-rate-mps", HOST_OPT_U32, &cfg.tcp_rate_mps, "TCP messages per second" },
    { "tcp-payload-len", HOST_OPT_U32, &cfg.tcp_payload_len, "TCP message payload (<=1024)" },
    { "tcp-count", HOST_OPT_U32, &cfg.tcp_count, "TCP messages to send, 0 = unlimited" },
    { "tcp-ack-timeout-ms", HOST_OPT_U32, &cfg.tcp_ack_timeout_ms, "stall detection" },
    { "tcp-connect-timeout-ms", HOST_OPT_U32, &cfg.tcp_connect_timeout_ms, "connect timeout" },
    { "tcp-backoff-min-ms", HOST_OPT_U32, &cfg.tcp_backoff_min_ms, "first reconnect delay" },
    { "tcp-backoff-max-ms", HOST_OPT_U32, &cfg.tcp_backoff_max_ms, "reconnect delay cap" },
    { "tcp-max-inflight", HOST_OPT_U32, &cfg.tcp_max_inflight, "unACKed message window" },
    { "relay-echo-rate-pps", HOST_OPT_U32, &cfg.relay_echo_rate_pps, "UDP rate towards relay" },
    { "report-interval-ms", HOST_OPT_U32, &cfg.report_interval_ms, "STATS period, 0 = final" },
    { "duration-ms", HOST_OPT_U32, &cfg.duration_ms, "run time, 0 = until SIGINT/SIGTERM" },
    { "udp", HOST_OPT_BOOL, &cfg.udp, "run the UDP echo client to node B" },
    { "tcp", HOST_OPT_BOOL, &cfg.tcp, "run the TCP stream client to node B" },
    { "relay-echo", HOST_OPT_BOOL, &cfg.relay_echo, "also run a UDP echo client to the relay" },
    { "udp-flow", HOST_OPT_STR, &cfg.udp_flow, "flow name of the UDP client" },
    { "tcp-flow", HOST_OPT_STR, &cfg.tcp_flow, "flow name of the TCP client" },
    { "relay-flow", HOST_OPT_STR, &cfg.relay_flow, "flow name of the relay UDP client" },
};

static struct udp_echo_client_cfg udp_cfg;
static struct udp_echo_client_cfg relay_cfg;
static struct tcp_stream_client_cfg tcp_cfg;

static void udp_task(void *arg)
{
    const struct udp_echo_client_cfg *c = (const struct udp_echo_client_cfg *)arg;
    int rc = udp_echo_client_run(c, NULL);
    if (rc != LAB_SVC_OK)
    {
        lab_log(LAB_LOG_ERR, "%s: udp_echo_client_run failed: %d", c->flow, rc);
    }
}

static void tcp_task(void *arg)
{
    (void)arg;
    int rc = tcp_stream_client_run(&tcp_cfg, NULL);
    if (rc != LAB_SVC_OK)
    {
        lab_log(LAB_LOG_ERR, "tcp_stream_client_run failed: %d", rc);
    }
}

static void fill_udp(struct udp_echo_client_cfg *c, const char *dst, uint32_t rate,
                     const char *flow)
{
    udp_echo_client_cfg_init(c);
    c->dst_ip = dst;
    c->dst_port = (uint16_t)cfg.udp_echo_port;
    c->rate_pps = rate;
    c->payload_len = (uint16_t)cfg.udp_payload_len;
    c->count = cfg.udp_count;
    c->duration_ms = cfg.duration_ms;
    c->drain_ms = cfg.udp_drain_ms;
    c->report_interval_ms = cfg.report_interval_ms;
    c->flow = flow;
    c->stop = &host_app_stop;
}

int main(int argc, char **argv)
{
    if (host_app_parse(argc, argv, "node_a_host", opts, sizeof(opts) / sizeof(opts[0])) != 0)
    {
        return 1;
    }
    host_app_init();
    uint64_t start_us = lab_time_us();

    lab_outf("EVENT,node_a,t_ms=0,ev=boot,dst=%s,relay=%s,udp=%d,tcp=%d,relay_echo=%d",
             cfg.ip_node_b, cfg.ip_relay, cfg.udp, cfg.tcp, cfg.relay_echo);

    if (cfg.udp)
    {
        fill_udp(&udp_cfg, cfg.ip_node_b, cfg.udp_rate_pps, cfg.udp_flow);
        if (host_app_spawn(udp_task, &udp_cfg, "udp_cli") != 0)
        {
            return 1;
        }
    }
    if (cfg.relay_echo)
    {
        fill_udp(&relay_cfg, cfg.ip_relay, cfg.relay_echo_rate_pps, cfg.relay_flow);
        if (host_app_spawn(udp_task, &relay_cfg, "relay_cli") != 0)
        {
            return 1;
        }
    }
    if (cfg.tcp)
    {
        tcp_stream_client_cfg_init(&tcp_cfg);
        tcp_cfg.dst_ip = cfg.ip_node_b;
        tcp_cfg.dst_port = (uint16_t)cfg.tcp_stream_port;
        tcp_cfg.rate_mps = cfg.tcp_rate_mps;
        tcp_cfg.payload_len = (uint16_t)cfg.tcp_payload_len;
        tcp_cfg.count = cfg.tcp_count;
        tcp_cfg.duration_ms = cfg.duration_ms;
        tcp_cfg.report_interval_ms = cfg.report_interval_ms;
        tcp_cfg.ack_timeout_ms = cfg.tcp_ack_timeout_ms;
        tcp_cfg.connect_timeout_ms = cfg.tcp_connect_timeout_ms;
        tcp_cfg.backoff_min_ms = cfg.tcp_backoff_min_ms;
        tcp_cfg.backoff_max_ms = cfg.tcp_backoff_max_ms;
        tcp_cfg.max_inflight = cfg.tcp_max_inflight;
        tcp_cfg.flow = cfg.tcp_flow;
        tcp_cfg.stop = &host_app_stop;
        if (host_app_spawn(tcp_task, NULL, "tcp_cli") != 0)
        {
            return 1;
        }
    }

    /*
     * The services end on their own after duration_ms (draining late replies and final ACKs);
     * SIGINT/SIGTERM stops them early.
     */
    host_app_wait(0);
    lab_outf("EVENT,node_a,t_ms=%lu,ev=exit",
             (unsigned long)((lab_time_us() - start_us) / 1000U));
    return 0;
}
