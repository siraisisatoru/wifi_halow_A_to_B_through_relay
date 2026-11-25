/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>

#include "halow_lab_config.h"
#include "lab_platform.h"
#include "lab_services.h"
#include "lab_sock.h"
#include "lab_util.h"
#include "linkproto.h"
#include "linkstats.h"
#include "svc_common.h"

/* Slack above the largest valid packet so oversized datagrams are detected, not truncated. */
#define UDP_RX_BUF_LEN (LINKPROTO_MAX_PACKET + 16)

/* Datagrams handled per wakeup before timers are serviced again. */
#define UDP_RX_BURST 32

#define UDP_MAX_RATE 10000

/* ------------------------------------------------------------------------------------------ */
/* Server                                                                                      */
/* ------------------------------------------------------------------------------------------ */

struct udp_srv
{
    const struct udp_echo_server_cfg *cfg;
    int fd;
    uint64_t start_us;
    uint8_t rx[UDP_RX_BUF_LEN];
    uint8_t tx[LINKPROTO_MAX_PACKET];

    /* Current client session (uplink statistics). */
    bool active;
    uint32_t session;
    struct sockaddr_in peer;
    struct linkstats up;
    uint32_t bad;
    uint32_t tx_err;

    /* The session that the client ended with a LAST request. Requests that still arrive for it
     * (a reordered or duplicated datagram) are echoed but do not start a new session. */
    bool have_ended;
    uint32_t ended_session;
    struct sockaddr_in ended_peer;
    uint32_t post_end;
};

void udp_echo_server_cfg_init(struct udp_echo_server_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = LAB_UDP_ECHO_PORT;
    cfg->report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg->flow = "udp_srv";
}

static void udp_srv_report(struct udp_srv *s, uint64_t now, bool final)
{
    char ip[LAB_IPV4_STR_MAX];
    char extra[128];
    char line[LAB_LINE_MAX];
    size_t pos = 0;

    lab_appendf(extra,
                sizeof(extra),
                &pos,
                ",peer=%s:%u,session=%08" PRIx32 ",bad=%" PRIu32 ",tx_err=%" PRIu32 ",final=%d",
                lab_sock_addr_str(&s->peer, ip, sizeof(ip)),
                (unsigned)lab_sock_addr_port(&s->peer),
                s->session,
                s->bad,
                s->tx_err,
                final ? 1 : 0);
    linkstats_format(&s->up, now, extra, line, sizeof(line));
    lab_out_line(line);
}

static void udp_srv_end_session(struct udp_srv *s, uint64_t now, const char *reason)
{
    if (!s->active)
    {
        return;
    }
    udp_srv_report(s, now, true);
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=session_end,session=%08" PRIx32 ",reason=%s",
             s->cfg->flow,
             svc_ms_since(s->start_us, now),
             s->session,
             reason);
    s->active = false;
}

/* Echo a request back to its sender; returns true if the whole reply was sent. */
static bool udp_srv_echo(struct udp_srv *s,
                         struct linkproto_hdr *hdr,
                         const uint8_t *payload,
                         const struct sockaddr_in *from)
{
    size_t out_len = 0;

    hdr->type = LINKPROTO_TYPE_ECHO_RSP;
    hdr->status = LINKPROTO_STATUS_OK;
    return linkproto_encode(hdr, payload, s->tx, sizeof(s->tx), &out_len) == LINKPROTO_OK &&
           sendto(s->fd,
                  s->tx,
                  out_len,
                  0,
                  (const struct sockaddr *)from,
                  (socklen_t)sizeof(*from)) == (int)out_len;
}

static void udp_srv_handle(struct udp_srv *s, size_t len, const struct sockaddr_in *from)
{
    struct linkproto_hdr hdr;
    const uint8_t *payload = NULL;
    uint64_t now = lab_time_us();

    enum linkproto_result r = linkproto_decode(s->rx, len, &hdr, &payload);
    if (r != LINKPROTO_OK || hdr.type != LINKPROTO_TYPE_ECHO_REQ)
    {
        /* Counted against the current session; garbage cannot be attributed reliably. */
        s->bad++;
        return;
    }

    if (s->have_ended && hdr.session == s->ended_session &&
        lab_sock_addr_equal(from, &s->ended_peer))
    {
        /* The client may still be waiting for this echo, but its statistics are final. */
        s->post_end++;
        bool echoed = udp_srv_echo(s, &hdr, payload, from);
        lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=post_end,session=%08" PRIx32 ",seq=%" PRIu32
                 ",count=%" PRIu32 ",echoed=%d",
                 s->cfg->flow,
                 svc_ms_since(s->start_us, now),
                 hdr.session,
                 hdr.seq,
                 s->post_end,
                 echoed ? 1 : 0);
        return;
    }

    if (!s->active || hdr.session != s->session || !lab_sock_addr_equal(from, &s->peer))
    {
        char ip[LAB_IPV4_STR_MAX];

        udp_srv_end_session(s, now, "new_session");
        linkstats_init(&s->up, s->cfg->flow, s->start_us);
        s->active = true;
        s->session = hdr.session;
        s->peer = *from;
        s->bad = 0;
        s->tx_err = 0;
        lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=session,peer=%s:%u,session=%08" PRIx32
                 ",first_seq=%" PRIu32,
                 s->cfg->flow,
                 svc_ms_since(s->start_us, now),
                 lab_sock_addr_str(from, ip, sizeof(ip)),
                 (unsigned)lab_sock_addr_port(from),
                 hdr.session,
                 hdr.seq);
    }

    enum linkstats_rx_class cls = linkstats_on_rx(&s->up, hdr.seq);
    if (cls == LINKSTATS_RX_NEW || cls == LINKSTATS_RX_LATE)
    {
        linkstats_on_transit(&s->up, (int64_t)now - (int64_t)hdr.ts_us);
    }

    if (udp_srv_echo(s, &hdr, payload, from))
    {
        linkstats_on_sent(&s->up);
    }
    else
    {
        s->tx_err++;
    }

    if (hdr.flags & LINKPROTO_FLAG_LAST)
    {
        udp_srv_end_session(s, now, "last");
        s->have_ended = true;
        s->ended_session = hdr.session;
        s->ended_peer = *from;
        s->post_end = 0;
    }
}

int udp_echo_server_run(const struct udp_echo_server_cfg *cfg)
{
    if (cfg == NULL || cfg->flow == NULL)
    {
        return LAB_SVC_ERR_CFG;
    }

    struct sockaddr_in bind_addr;
    if (lab_sock_make_addr("0.0.0.0", cfg->port, &bind_addr) != 0)
    {
        return LAB_SVC_ERR_CFG;
    }

    struct udp_srv *s = (struct udp_srv *)lab_malloc(sizeof(*s));
    if (s == NULL)
    {
        return LAB_SVC_ERR_NOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
    s->start_us = lab_time_us();

    s->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->fd < 0)
    {
        lab_log(LAB_LOG_ERR, "%s: socket() failed: %d", cfg->flow, lab_sock_errno());
        lab_free(s);
        return LAB_SVC_ERR_SOCKET;
    }
    lab_sock_set_reuseaddr(s->fd);
    if (bind(s->fd, (const struct sockaddr *)&bind_addr, (socklen_t)sizeof(bind_addr)) != 0 ||
        lab_sock_set_nonblocking(s->fd) != 0)
    {
        lab_log(LAB_LOG_ERR, "%s: bind(:%u) failed: %d", cfg->flow, cfg->port, lab_sock_errno());
        lab_sock_close(s->fd);
        lab_free(s);
        return LAB_SVC_ERR_SOCKET;
    }

    lab_outf("EVENT,%s,t_ms=0,ev=listening,proto=udp,port=%u", cfg->flow, (unsigned)cfg->port);

    uint64_t report_us = (uint64_t)cfg->report_interval_ms * 1000U;
    uint64_t next_report = s->start_us + report_us;

    while (!svc_stop_requested(cfg->stop))
    {
        uint64_t now = lab_time_us();
        uint32_t timeout_ms = LAB_SVC_POLL_MS;
        if (report_us != 0 && s->active)
        {
            svc_clamp_timeout(&timeout_ms, now, next_report);
        }

        int ready = lab_sock_wait(s->fd, LAB_WAIT_READ, timeout_ms);
        if (ready < 0)
        {
            lab_log(LAB_LOG_WRN, "%s: select failed: %d", cfg->flow, lab_sock_errno());
            lab_sleep_ms(LAB_SVC_POLL_MS);
        }
        else if (ready & LAB_WAIT_READ)
        {
            for (int i = 0; i < UDP_RX_BURST; i++)
            {
                struct sockaddr_in from;
                socklen_t from_len = (socklen_t)sizeof(from);
                int n = recvfrom(s->fd,
                                 s->rx,
                                 sizeof(s->rx),
                                 MSG_DONTWAIT,
                                 (struct sockaddr *)&from,
                                 &from_len);
                if (n < 0)
                {
                    int err = lab_sock_errno();
                    if (!lab_sock_retryable(err))
                    {
                        lab_log(LAB_LOG_WRN, "%s: recvfrom failed: %d", cfg->flow, err);
                    }
                    break;
                }
                udp_srv_handle(s, (size_t)n, &from);
            }
        }

        now = lab_time_us();
        if (report_us != 0 && now >= next_report)
        {
            if (s->active)
            {
                udp_srv_report(s, now, false);
            }
            next_report += report_us;
            if (next_report <= now)
            {
                next_report = now + report_us;
            }
        }
    }

    uint64_t now = lab_time_us();
    udp_srv_end_session(s, now, "stopped");
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=stopped", cfg->flow, svc_ms_since(s->start_us, now));
    lab_sock_close(s->fd);
    lab_free(s);
    return LAB_SVC_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Client                                                                                      */
/* ------------------------------------------------------------------------------------------ */

struct udp_cli
{
    const struct udp_echo_client_cfg *cfg;
    int fd;
    struct sockaddr_in dst;
    uint32_t session;
    uint64_t start_us;
    uint32_t next_seq;
    struct linkstats st;
    uint32_t tx_err;
    uint32_t bad;
    uint32_t foreign;
    uint32_t corrupt;
    uint8_t tx[LINKPROTO_MAX_PACKET];
    uint8_t rx[UDP_RX_BUF_LEN];
};

void udp_echo_client_cfg_init(struct udp_echo_client_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->dst_ip = LAB_IP_NODE_B;
    cfg->dst_port = LAB_UDP_ECHO_PORT;
    cfg->rate_pps = LAB_UDP_RATE_PPS;
    cfg->payload_len = LAB_UDP_PAYLOAD_LEN;
    cfg->report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg->drain_ms = LAB_UDP_DRAIN_MS;
    cfg->flow = "udp";
}

static void udp_cli_report(struct udp_cli *c, uint64_t now, bool final)
{
    char extra[128];
    char line[LAB_LINE_MAX];
    size_t pos = 0;

    lab_appendf(extra,
                sizeof(extra),
                &pos,
                ",tx_err=%" PRIu32 ",bad=%" PRIu32 ",foreign=%" PRIu32 ",corrupt=%" PRIu32
                ",final=%d",
                c->tx_err,
                c->bad,
                c->foreign,
                c->corrupt,
                final ? 1 : 0);
    linkstats_format(&c->st, now, extra, line, sizeof(line));
    lab_out_line(line);
}

static void udp_cli_send(struct udp_cli *c, uint64_t now)
{
    const struct udp_echo_client_cfg *cfg = c->cfg;
    struct linkproto_hdr hdr;

    linkproto_hdr_init(&hdr, LINKPROTO_TYPE_ECHO_REQ, c->next_seq, c->session, now);
    hdr.payload_len = cfg->payload_len;
    if (cfg->count != 0 && c->next_seq == cfg->count - 1U)
    {
        hdr.flags |= LINKPROTO_FLAG_LAST;
    }
    linkproto_fill_pattern(c->tx + LINKPROTO_HDR_LEN, cfg->payload_len, c->next_seq);

    size_t len = 0;
    if (linkproto_encode(&hdr, NULL, c->tx, sizeof(c->tx), &len) != LINKPROTO_OK ||
        sendto(c->fd,
               c->tx,
               len,
               0,
               (const struct sockaddr *)&c->dst,
               (socklen_t)sizeof(c->dst)) != (int)len)
    {
        /* Still a sent seq: it shows up as lost, tx_err tells why. */
        c->tx_err++;
    }
    linkstats_on_sent(&c->st);
    c->next_seq++;
}

static void udp_cli_handle(struct udp_cli *c, size_t len, const struct sockaddr_in *from)
{
    struct linkproto_hdr hdr;
    const uint8_t *payload = NULL;
    uint64_t now = lab_time_us();

    if (!lab_sock_addr_equal(from, &c->dst))
    {
        c->foreign++;
        return;
    }
    if (linkproto_decode(c->rx, len, &hdr, &payload) != LINKPROTO_OK ||
        hdr.type != LINKPROTO_TYPE_ECHO_RSP)
    {
        c->bad++;
        return;
    }
    if (hdr.session != c->session)
    {
        c->foreign++;
        return;
    }
    /* An echo of a seq not sent yet, or from the future, is not ours to account. */
    if ((int32_t)(hdr.seq - c->next_seq) >= 0 || hdr.ts_us > now)
    {
        c->bad++;
        return;
    }
    if (hdr.payload_len != c->cfg->payload_len ||
        !linkproto_check_pattern(payload, hdr.payload_len, hdr.seq))
    {
        c->corrupt++;
        return;
    }

    enum linkstats_rx_class cls = linkstats_on_rx(&c->st, hdr.seq);
    if (cls == LINKSTATS_RX_NEW || cls == LINKSTATS_RX_LATE)
    {
        uint64_t rtt = now - hdr.ts_us;
        linkstats_on_rtt(&c->st, rtt > UINT32_MAX ? UINT32_MAX : (uint32_t)rtt);
    }
}

static void udp_cli_drain_rx(struct udp_cli *c)
{
    for (int i = 0; i < UDP_RX_BURST; i++)
    {
        struct sockaddr_in from;
        socklen_t from_len = (socklen_t)sizeof(from);
        int n = recvfrom(c->fd,
                         c->rx,
                         sizeof(c->rx),
                         MSG_DONTWAIT,
                         (struct sockaddr *)&from,
                         &from_len);
        if (n < 0)
        {
            int err = lab_sock_errno();
            if (!lab_sock_retryable(err))
            {
                /* e.g. ECONNREFUSED from an ICMP port unreachable on Linux. */
                c->bad++;
            }
            break;
        }
        udp_cli_handle(c, (size_t)n, &from);
    }
}

int udp_echo_client_run(const struct udp_echo_client_cfg *cfg, struct linkstats *result)
{
    if (cfg == NULL || cfg->flow == NULL || cfg->rate_pps == 0 || cfg->rate_pps > UDP_MAX_RATE ||
        cfg->payload_len > LINKPROTO_MAX_PAYLOAD)
    {
        return LAB_SVC_ERR_CFG;
    }

    struct udp_cli *c = (struct udp_cli *)lab_malloc(sizeof(*c));
    if (c == NULL)
    {
        return LAB_SVC_ERR_NOMEM;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = cfg;
    if (lab_sock_make_addr(cfg->dst_ip, cfg->dst_port, &c->dst) != 0)
    {
        lab_log(LAB_LOG_ERR, "%s: bad destination '%s'", cfg->flow, cfg->dst_ip ? cfg->dst_ip : "");
        lab_free(c);
        return LAB_SVC_ERR_CFG;
    }

    c->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (c->fd < 0 || lab_sock_set_nonblocking(c->fd) != 0)
    {
        lab_log(LAB_LOG_ERR, "%s: socket() failed: %d", cfg->flow, lab_sock_errno());
        lab_sock_close(c->fd);
        lab_free(c);
        return LAB_SVC_ERR_SOCKET;
    }

    c->session = lab_random_u32();
    c->start_us = lab_time_us();
    linkstats_init(&c->st, cfg->flow, c->start_us);
    linkstats_set_first_seq(&c->st, 0);

    char ip[LAB_IPV4_STR_MAX];
    lab_outf("EVENT,%s,t_ms=0,ev=start,dst=%s:%u,rate_pps=%" PRIu32 ",payload=%u,count=%" PRIu32
             ",duration_ms=%" PRIu32 ",session=%08" PRIx32,
             cfg->flow,
             lab_sock_addr_str(&c->dst, ip, sizeof(ip)),
             (unsigned)cfg->dst_port,
             cfg->rate_pps,
             (unsigned)cfg->payload_len,
             cfg->count,
             cfg->duration_ms,
             c->session);

    const uint64_t period_us = 1000000U / cfg->rate_pps;
    const uint64_t report_us = (uint64_t)cfg->report_interval_ms * 1000U;
    uint64_t next_send = c->start_us;
    uint64_t next_report = c->start_us + report_us;
    uint64_t drain_deadline = 0;
    bool sending = true;
    const char *end_reason = "done";

    for (;;)
    {
        uint64_t now = lab_time_us();

        if (svc_stop_requested(cfg->stop))
        {
            end_reason = "stopped";
            break;
        }
        if (sending && ((cfg->count != 0 && c->next_seq >= cfg->count) ||
                        (cfg->duration_ms != 0 &&
                         now - c->start_us >= (uint64_t)cfg->duration_ms * 1000U)))
        {
            sending = false;
            drain_deadline = now + (uint64_t)cfg->drain_ms * 1000U;
        }
        if (!sending && now >= drain_deadline)
        {
            break;
        }

        if (sending && now >= next_send)
        {
            udp_cli_send(c, now);
            next_send += period_us;
            /* After a long stall (e.g. blocked sendto) resume the rate instead of bursting. */
            if (now >= next_send + 1000000U)
            {
                next_send = now + period_us;
            }
        }

        uint32_t timeout_ms = LAB_SVC_POLL_MS;
        if (sending)
        {
            svc_clamp_timeout(&timeout_ms, now, next_send);
        }
        else
        {
            svc_clamp_timeout(&timeout_ms, now, drain_deadline);
        }
        if (report_us != 0)
        {
            svc_clamp_timeout(&timeout_ms, now, next_report);
        }

        int ready = lab_sock_wait(c->fd, LAB_WAIT_READ, timeout_ms);
        if (ready > 0 && (ready & LAB_WAIT_READ))
        {
            udp_cli_drain_rx(c);
        }
        else if (ready < 0)
        {
            lab_sleep_ms(1);
        }

        now = lab_time_us();
        if (report_us != 0 && now >= next_report)
        {
            udp_cli_report(c, now, false);
            next_report += report_us;
            if (next_report <= now)
            {
                next_report = now + report_us;
            }
        }
    }

    uint64_t now = lab_time_us();
    linkstats_finalize(&c->st, c->next_seq);
    udp_cli_report(c, now, true);
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=end,reason=%s,sent=%" PRIu32,
             cfg->flow,
             svc_ms_since(c->start_us, now),
             end_reason,
             c->st.sent);

    if (result != NULL)
    {
        *result = c->st;
    }
    lab_sock_close(c->fd);
    lab_free(c);
    return LAB_SVC_OK;
}
