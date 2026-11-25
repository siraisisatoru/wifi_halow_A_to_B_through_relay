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
#include "msgstream.h"
#include "svc_common.h"

#define TCP_RX_CHUNK 512

/* ACK frames carry no payload. */
#define TCP_ACK_FRAME_LEN MSGSTREAM_MIN_FRAME

/* Server ACK queue: enough for a full read chunk of minimum-size MSG frames. */
#define TCP_SRV_TXQ_LEN (TCP_ACK_FRAME_LEN * 64)

#define TCP_MAX_RATE 10000

/* ------------------------------------------------------------------------------------------ */
/* Server                                                                                      */
/* ------------------------------------------------------------------------------------------ */

struct tcp_srv
{
    const struct tcp_stream_server_cfg *cfg;
    int listen_fd;
    int conn_fd;
    uint64_t start_us;
    struct sockaddr_in peer;
    uint64_t conn_since_us;
    uint64_t last_rx_us;

    struct msgstream_parser parser;
    uint8_t txq[TCP_SRV_TXQ_LEN];
    size_t txq_len;
    uint8_t rx[TCP_RX_CHUNK];

    /* Current client session; survives reconnects of the same session. */
    bool active;
    uint32_t session;
    struct linkstats st;
    uint32_t accepts;
    uint32_t bad_type;
    uint32_t bad_payload;
    uint32_t ack_drop;
};

void tcp_stream_server_cfg_init(struct tcp_stream_server_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = LAB_TCP_STREAM_PORT;
    cfg->report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg->idle_timeout_ms = LAB_TCP_SERVER_IDLE_TIMEOUT_MS;
    cfg->flow = "tcp_srv";
}

static void tcp_srv_report(struct tcp_srv *s, uint64_t now, bool final)
{
    const struct msgstream_stats *ps = &s->parser.stats;
    char extra[200];
    char line[LAB_LINE_MAX];
    size_t pos = 0;

    lab_appendf(extra,
                sizeof(extra),
                &pos,
                ",session=%08" PRIx32 ",up=%d,accepts=%" PRIu32 ",bad_type=%" PRIu32
                ",bad_payload=%" PRIu32 ",crc_err=%" PRIu32 ",hdr_err=%" PRIu32
                ",len_err=%" PRIu32 ",resyncs=%" PRIu32 ",discarded=%" PRIu32
                ",ack_drop=%" PRIu32 ",final=%d",
                s->session,
                s->conn_fd >= 0 ? 1 : 0,
                s->accepts,
                s->bad_type,
                s->bad_payload,
                ps->crc_errors,
                ps->hdr_errors,
                ps->len_errors,
                ps->resyncs,
                ps->bytes_discarded,
                s->ack_drop,
                final ? 1 : 0);
    linkstats_format(&s->st, now, extra, line, sizeof(line));
    lab_out_line(line);
}

static void tcp_srv_end_session(struct tcp_srv *s, uint64_t now, const char *reason)
{
    if (!s->active)
    {
        return;
    }
    tcp_srv_report(s, now, true);
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=session_end,session=%08" PRIx32 ",reason=%s",
             s->cfg->flow,
             svc_ms_since(s->start_us, now),
             s->session,
             reason);
    s->active = false;
}

static void tcp_srv_close_conn(struct tcp_srv *s, uint64_t now, const char *reason, int err)
{
    if (s->conn_fd < 0)
    {
        return;
    }
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=closed,reason=%s,err=%d,up_ms=%" PRIu32,
             s->cfg->flow,
             svc_ms_since(s->start_us, now),
             reason,
             err,
             svc_ms_since(s->conn_since_us, now));
    lab_sock_close(s->conn_fd);
    s->conn_fd = -1;
    s->txq_len = 0;
    msgstream_parser_reset(&s->parser);
}

static void tcp_srv_on_frame(void *ctx, const struct linkproto_hdr *hdr, const uint8_t *payload)
{
    struct tcp_srv *s = (struct tcp_srv *)ctx;
    uint64_t now = lab_time_us();

    if (hdr->type != LINKPROTO_TYPE_MSG)
    {
        s->bad_type++;
        return;
    }

    if (!s->active || hdr->session != s->session)
    {
        tcp_srv_end_session(s, now, "new_session");
        linkstats_init(&s->st, s->cfg->flow, s->start_us);
        s->active = true;
        s->session = hdr->session;
        s->bad_type = 0;
        s->bad_payload = 0;
        s->ack_drop = 0;
        lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=session,session=%08" PRIx32 ",first_seq=%" PRIu32,
                 s->cfg->flow,
                 svc_ms_since(s->start_us, now),
                 hdr->session,
                 hdr->seq);
    }

    enum linkstats_rx_class cls = linkstats_on_rx(&s->st, hdr->seq);
    if (cls == LINKSTATS_RX_NEW || cls == LINKSTATS_RX_LATE)
    {
        linkstats_on_transit(&s->st, (int64_t)now - (int64_t)hdr->ts_us);
    }

    struct linkproto_hdr ack;
    linkproto_hdr_init(&ack, LINKPROTO_TYPE_MSG_ACK, hdr->seq, hdr->session, hdr->ts_us);
    ack.flags = hdr->flags & LINKPROTO_FLAG_LAST;
    ack.status = LINKPROTO_STATUS_OK;
    if (!linkproto_check_pattern(payload, hdr->payload_len, hdr->seq))
    {
        ack.status = LINKPROTO_STATUS_BAD_PAYLOAD;
        s->bad_payload++;
    }

    size_t n = 0;
    if (s->txq_len + TCP_ACK_FRAME_LEN <= sizeof(s->txq) &&
        msgstream_encode(&ack, NULL, s->txq + s->txq_len, sizeof(s->txq) - s->txq_len, &n) ==
            LINKPROTO_OK)
    {
        s->txq_len += n;
        linkstats_on_sent(&s->st);
    }
    else
    {
        s->ack_drop++;
    }

    if (hdr->flags & LINKPROTO_FLAG_LAST)
    {
        tcp_srv_end_session(s, now, "last");
    }
}

static void tcp_srv_flush(struct tcp_srv *s, uint64_t now)
{
    while (s->conn_fd >= 0 && s->txq_len > 0)
    {
        int n = send(s->conn_fd, s->txq, s->txq_len, LAB_SEND_FLAGS);
        if (n > 0)
        {
            memmove(s->txq, s->txq + n, s->txq_len - (size_t)n);
            s->txq_len -= (size_t)n;
            continue;
        }
        int err = lab_sock_errno();
        if (n < 0 && !lab_sock_retryable(err))
        {
            tcp_srv_close_conn(s, now, "send_error", err);
        }
        break;
    }
}

/* Bytes that may be read without the resulting ACKs overflowing the queue. */
static size_t tcp_srv_read_budget(const struct tcp_srv *s)
{
    size_t acks = (sizeof(s->txq) - s->txq_len) / TCP_ACK_FRAME_LEN;
    size_t bytes = acks * TCP_ACK_FRAME_LEN;
    if (bytes <= s->parser.fill)
    {
        return 0;
    }
    bytes -= s->parser.fill;
    return bytes < sizeof(s->rx) ? bytes : sizeof(s->rx);
}

static void tcp_srv_accept(struct tcp_srv *s, uint64_t now)
{
    struct sockaddr_in from;
    socklen_t from_len = (socklen_t)sizeof(from);
    int fd = accept(s->listen_fd, (struct sockaddr *)&from, &from_len);
    if (fd < 0)
    {
        return;
    }

    /* The newest connection wins: the old one is most likely half-open after a relay drop. */
    tcp_srv_close_conn(s, now, "replaced", 0);
    if (lab_sock_set_nonblocking(fd) != 0)
    {
        lab_sock_close(fd);
        return;
    }
    lab_sock_tune_tcp(fd);
    s->conn_fd = fd;
    s->peer = from;
    s->conn_since_us = now;
    s->last_rx_us = now;
    s->txq_len = 0;
    s->accepts++;
    msgstream_parser_reset(&s->parser);

    char ip[LAB_IPV4_STR_MAX];
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=accepted,peer=%s:%u,accepts=%" PRIu32,
             s->cfg->flow,
             svc_ms_since(s->start_us, now),
             lab_sock_addr_str(&from, ip, sizeof(ip)),
             (unsigned)lab_sock_addr_port(&from),
             s->accepts);
}

int tcp_stream_server_run(const struct tcp_stream_server_cfg *cfg)
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

    struct tcp_srv *s = (struct tcp_srv *)lab_malloc(sizeof(*s));
    if (s == NULL)
    {
        return LAB_SVC_ERR_NOMEM;
    }
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
    s->conn_fd = -1;
    s->start_us = lab_time_us();
    msgstream_parser_init(&s->parser);

    s->listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->listen_fd < 0)
    {
        lab_log(LAB_LOG_ERR, "%s: socket() failed: %d", cfg->flow, lab_sock_errno());
        lab_free(s);
        return LAB_SVC_ERR_SOCKET;
    }
    lab_sock_set_reuseaddr(s->listen_fd);
    if (bind(s->listen_fd, (const struct sockaddr *)&bind_addr, (socklen_t)sizeof(bind_addr)) !=
            0 ||
        listen(s->listen_fd, 2) != 0 || lab_sock_set_nonblocking(s->listen_fd) != 0)
    {
        lab_log(LAB_LOG_ERR, "%s: bind/listen(:%u) failed: %d", cfg->flow, cfg->port, lab_sock_errno());
        lab_sock_close(s->listen_fd);
        lab_free(s);
        return LAB_SVC_ERR_SOCKET;
    }

    lab_outf("EVENT,%s,t_ms=0,ev=listening,proto=tcp,port=%u", cfg->flow, (unsigned)cfg->port);

    const uint64_t report_us = (uint64_t)cfg->report_interval_ms * 1000U;
    const uint64_t idle_us = (uint64_t)cfg->idle_timeout_ms * 1000U;
    uint64_t next_report = s->start_us + report_us;

    while (!svc_stop_requested(cfg->stop))
    {
        uint64_t now = lab_time_us();
        size_t budget = tcp_srv_read_budget(s);
        struct lab_poll fds[2] = {
            { .fd = s->listen_fd, .events = LAB_WAIT_READ, .revents = 0 },
            { .fd = s->conn_fd,
              .events = (budget > 0 ? LAB_WAIT_READ : 0) | (s->txq_len > 0 ? LAB_WAIT_WRITE : 0),
              .revents = 0 },
        };
        uint32_t timeout_ms = LAB_SVC_POLL_MS;
        if (report_us != 0 && s->active)
        {
            svc_clamp_timeout(&timeout_ms, now, next_report);
        }

        if (lab_sock_poll(fds, 2, timeout_ms) < 0)
        {
            lab_log(LAB_LOG_WRN, "%s: select failed: %d", cfg->flow, lab_sock_errno());
            lab_sleep_ms(LAB_SVC_POLL_MS);
            continue;
        }
        now = lab_time_us();

        if (fds[0].revents & LAB_WAIT_READ)
        {
            tcp_srv_accept(s, now);
        }
        else if (s->conn_fd >= 0 && s->conn_fd == fds[1].fd)
        {
            if (fds[1].revents & LAB_WAIT_WRITE)
            {
                tcp_srv_flush(s, now);
            }
            if (s->conn_fd >= 0 && (fds[1].revents & LAB_WAIT_READ))
            {
                int n = recv(s->conn_fd, s->rx, budget, 0);
                if (n > 0)
                {
                    s->last_rx_us = now;
                    msgstream_parser_feed(&s->parser, s->rx, (size_t)n, tcp_srv_on_frame, s);
                    tcp_srv_flush(s, now);
                }
                else if (n == 0)
                {
                    tcp_srv_close_conn(s, now, "eof", 0);
                }
                else if (!lab_sock_retryable(lab_sock_errno()))
                {
                    tcp_srv_close_conn(s, now, "recv_error", lab_sock_errno());
                }
            }
        }

        if (s->conn_fd >= 0 && idle_us != 0 && now - s->last_rx_us >= idle_us)
        {
            tcp_srv_close_conn(s, now, "idle", 0);
        }

        if (report_us != 0 && now >= next_report)
        {
            if (s->active)
            {
                tcp_srv_report(s, now, false);
            }
            next_report += report_us;
            if (next_report <= now)
            {
                next_report = now + report_us;
            }
        }
    }

    uint64_t now = lab_time_us();
    tcp_srv_close_conn(s, now, "stopped", 0);
    tcp_srv_end_session(s, now, "stopped");
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=stopped", cfg->flow, svc_ms_since(s->start_us, now));
    lab_sock_close(s->listen_fd);
    lab_free(s);
    return LAB_SVC_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Client                                                                                      */
/* ------------------------------------------------------------------------------------------ */

enum tcp_cli_state
{
    TCP_CLI_DOWN,
    TCP_CLI_CONNECTING,
    TCP_CLI_UP,
};

struct tcp_cli
{
    const struct tcp_stream_client_cfg *cfg;
    struct sockaddr_in dst;
    uint32_t session;
    uint64_t start_us;

    enum tcp_cli_state state;
    int fd;
    /* Consecutive attempts without an acknowledged message (drives the backoff). */
    uint32_t fails;
    uint64_t retry_at_us;
    uint64_t connect_started_us;
    uint64_t up_since_us;
    uint64_t down_since_us;
    bool ever_up;

    uint32_t next_seq;
    /* Oldest unacknowledged seq; una == next_seq means nothing in flight. */
    uint32_t una;
    uint64_t next_send_us;
    uint64_t sent_us[LAB_TCP_MAX_INFLIGHT_LIMIT];

    uint8_t tx[MSGSTREAM_MAX_FRAME];
    size_t tx_len;
    size_t tx_off;
    uint8_t rx[TCP_RX_CHUNK];
    struct msgstream_parser parser;

    struct linkstats st;
    uint32_t connects;
    uint32_t disconnects;
    /* Messages that were in flight when a connection went down (counted lost by linkstats). */
    uint32_t abandoned;
    uint32_t stalls;
    uint32_t connect_fail;
    uint32_t bad_status;
    uint32_t proto_err;
};

void tcp_stream_client_cfg_init(struct tcp_stream_client_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->dst_ip = LAB_IP_NODE_B;
    cfg->dst_port = LAB_TCP_STREAM_PORT;
    cfg->rate_mps = LAB_TCP_RATE_MPS;
    cfg->payload_len = LAB_TCP_PAYLOAD_LEN;
    cfg->report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg->connect_timeout_ms = LAB_TCP_CONNECT_TIMEOUT_MS;
    cfg->ack_timeout_ms = LAB_TCP_ACK_TIMEOUT_MS;
    cfg->backoff_min_ms = LAB_TCP_BACKOFF_MIN_MS;
    cfg->backoff_max_ms = LAB_TCP_BACKOFF_MAX_MS;
    cfg->max_inflight = LAB_TCP_MAX_INFLIGHT;
    cfg->flow = "tcp";
}

static uint32_t tcp_cli_backoff_ms(const struct tcp_cli *c)
{
    uint32_t lo = c->cfg->backoff_min_ms > 0 ? c->cfg->backoff_min_ms : 1;
    uint32_t hi = c->cfg->backoff_max_ms > lo ? c->cfg->backoff_max_ms : lo;
    uint32_t d = lo;

    for (uint32_t i = 1; i < c->fails && d < hi; i++)
    {
        d = d > hi / 2 ? hi : d * 2;
    }
    if (d > hi)
    {
        d = hi;
    }
    /* "Equal jitter": uniformly in [d/2, d] so reconnecting clients do not synchronise. */
    return d / 2 + lab_random_u32() % (d - d / 2 + 1);
}

static uint32_t tcp_cli_inflight(const struct tcp_cli *c)
{
    return c->next_seq - c->una;
}

static void tcp_cli_report(struct tcp_cli *c, uint64_t now, bool final)
{
    char extra[200];
    char line[LAB_LINE_MAX];
    size_t pos = 0;

    lab_appendf(extra,
                sizeof(extra),
                &pos,
                ",up=%d,connects=%" PRIu32 ",disconnects=%" PRIu32 ",stalls=%" PRIu32
                ",connect_fail=%" PRIu32 ",inflight=%" PRIu32 ",bad_status=%" PRIu32
                ",proto_err=%" PRIu32 ",crc_err=%" PRIu32 ",resyncs=%" PRIu32 ",final=%d",
                c->state == TCP_CLI_UP ? 1 : 0,
                c->connects,
                c->disconnects,
                c->stalls,
                c->connect_fail,
                tcp_cli_inflight(c),
                c->bad_status,
                c->proto_err,
                c->parser.stats.crc_errors,
                c->parser.stats.resyncs,
                final ? 1 : 0);
    linkstats_format(&c->st, now, extra, line, sizeof(line));
    lab_out_line(line);
}

static void tcp_cli_go_down(struct tcp_cli *c, uint64_t now)
{
    lab_sock_close(c->fd);
    c->fd = -1;
    c->state = TCP_CLI_DOWN;
    c->fails++;
    c->retry_at_us = now + (uint64_t)tcp_cli_backoff_ms(c) * 1000U;
    /* Unacked messages are abandoned (counted lost); seq numbering continues. */
    c->abandoned += tcp_cli_inflight(c);
    c->una = c->next_seq;
    c->tx_len = 0;
    c->tx_off = 0;
    msgstream_parser_reset(&c->parser);
}

static void tcp_cli_disconnect(struct tcp_cli *c, uint64_t now, const char *reason, int err)
{
    uint32_t unacked = tcp_cli_inflight(c);
    uint32_t age_ms = unacked > 0 ?
                          svc_ms_since(c->sent_us[c->una % LAB_TCP_MAX_INFLIGHT_LIMIT], now) :
                          0;

    c->disconnects++;
    c->down_since_us = now;
    tcp_cli_go_down(c, now);
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=disconnected,reason=%s,err=%d,unacked=%" PRIu32
             ",oldest_age_ms=%" PRIu32 ",up_ms=%" PRIu32 ",retry_ms=%" PRIu32,
             c->cfg->flow,
             svc_ms_since(c->start_us, now),
             reason,
             err,
             unacked,
             age_ms,
             svc_ms_since(c->up_since_us, now),
             svc_ms_since(now, c->retry_at_us));
}

static void tcp_cli_connect_failed(struct tcp_cli *c, uint64_t now, int err)
{
    uint32_t attempt = c->fails + 1U;

    c->connect_fail++;
    tcp_cli_go_down(c, now);
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=connect_failed,attempt=%" PRIu32 ",err=%d"
             ",retry_ms=%" PRIu32,
             c->cfg->flow,
             svc_ms_since(c->start_us, now),
             attempt,
             err,
             svc_ms_since(now, c->retry_at_us));
}

static void tcp_cli_connected(struct tcp_cli *c, uint64_t now)
{
    c->state = TCP_CLI_UP;
    c->connects++;
    c->up_since_us = now;
    c->next_send_us = now;
    c->una = c->next_seq;
    c->tx_len = 0;
    c->tx_off = 0;
    msgstream_parser_reset(&c->parser);

    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=connected,attempt=%" PRIu32 ",connect_ms=%" PRIu32
             ",outage_ms=%" PRIu32 ",next_seq=%" PRIu32,
             c->cfg->flow,
             svc_ms_since(c->start_us, now),
             c->fails + 1U,
             svc_ms_since(c->connect_started_us, now),
             c->ever_up ? svc_ms_since(c->down_since_us, now) : 0,
             c->next_seq);
    c->ever_up = true;
}

static void tcp_cli_start_connect(struct tcp_cli *c, uint64_t now)
{
    c->connect_started_us = now;
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=connecting,attempt=%" PRIu32,
             c->cfg->flow,
             svc_ms_since(c->start_us, now),
             c->fails + 1U);

    c->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->fd < 0)
    {
        tcp_cli_connect_failed(c, now, lab_sock_errno());
        return;
    }
    if (lab_sock_set_nonblocking(c->fd) != 0)
    {
        tcp_cli_connect_failed(c, now, lab_sock_errno());
        return;
    }
    lab_sock_tune_tcp(c->fd);

    if (connect(c->fd, (const struct sockaddr *)&c->dst, (socklen_t)sizeof(c->dst)) == 0)
    {
        tcp_cli_connected(c, now);
        return;
    }
    int err = lab_sock_errno();
    if (err == EINPROGRESS || lab_sock_retryable(err))
    {
        c->state = TCP_CLI_CONNECTING;
        return;
    }
    tcp_cli_connect_failed(c, now, err);
}

static void tcp_cli_check_connect(struct tcp_cli *c, uint64_t now)
{
    int err = 0;
    socklen_t len = (socklen_t)sizeof(err);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
    {
        err = lab_sock_errno();
    }
    if (err == 0)
    {
        tcp_cli_connected(c, now);
    }
    else
    {
        tcp_cli_connect_failed(c, now, err);
    }
}

static void tcp_cli_on_frame(void *ctx, const struct linkproto_hdr *hdr, const uint8_t *payload)
{
    struct tcp_cli *c = (struct tcp_cli *)ctx;
    uint64_t now = lab_time_us();

    (void)payload;
    if (hdr->type != LINKPROTO_TYPE_MSG_ACK || hdr->session != c->session ||
        (int32_t)(hdr->seq - c->next_seq) >= 0 || hdr->ts_us > now)
    {
        c->proto_err++;
        return;
    }
    if (hdr->status != LINKPROTO_STATUS_OK)
    {
        c->bad_status++;
    }

    enum linkstats_rx_class cls = linkstats_on_rx(&c->st, hdr->seq);
    if (cls == LINKSTATS_RX_NEW || cls == LINKSTATS_RX_LATE)
    {
        uint64_t rtt = now - hdr->ts_us;
        linkstats_on_rtt(&c->st, rtt > UINT32_MAX ? UINT32_MAX : (uint32_t)rtt);
    }
    if ((int32_t)(hdr->seq - c->una) >= 0)
    {
        c->una = hdr->seq + 1U;
    }
    /* The path works end to end: restart the backoff sequence. */
    c->fails = 0;
}

static void tcp_cli_queue_msg(struct tcp_cli *c, uint64_t now)
{
    const struct tcp_stream_client_cfg *cfg = c->cfg;
    struct linkproto_hdr hdr;
    uint8_t *payload = c->tx + MSGSTREAM_PREFIX_LEN + LINKPROTO_HDR_LEN;

    linkproto_hdr_init(&hdr, LINKPROTO_TYPE_MSG, c->next_seq, c->session, now);
    hdr.payload_len = cfg->payload_len;
    if (cfg->count != 0 && c->next_seq == cfg->count - 1U)
    {
        hdr.flags |= LINKPROTO_FLAG_LAST;
    }
    linkproto_fill_pattern(payload, cfg->payload_len, c->next_seq);
    if (msgstream_encode(&hdr, NULL, c->tx, sizeof(c->tx), &c->tx_len) != LINKPROTO_OK)
    {
        c->tx_len = 0;
        return;
    }
    c->tx_off = 0;
    c->sent_us[c->next_seq % LAB_TCP_MAX_INFLIGHT_LIMIT] = now;
    c->next_seq++;
    linkstats_on_sent(&c->st);
}

static void tcp_cli_flush(struct tcp_cli *c, uint64_t now)
{
    while (c->state == TCP_CLI_UP && c->tx_off < c->tx_len)
    {
        int n = send(c->fd, c->tx + c->tx_off, c->tx_len - c->tx_off, LAB_SEND_FLAGS);
        if (n > 0)
        {
            c->tx_off += (size_t)n;
            continue;
        }
        int err = lab_sock_errno();
        if (n < 0 && !lab_sock_retryable(err))
        {
            tcp_cli_disconnect(c, now, "send_error", err);
        }
        break;
    }
}

static void tcp_cli_read(struct tcp_cli *c, uint64_t now)
{
    int n = recv(c->fd, c->rx, sizeof(c->rx), 0);
    if (n > 0)
    {
        msgstream_parser_feed(&c->parser, c->rx, (size_t)n, tcp_cli_on_frame, c);
    }
    else if (n == 0)
    {
        tcp_cli_disconnect(c, now, "eof", 0);
    }
    else
    {
        int err = lab_sock_errno();
        if (!lab_sock_retryable(err))
        {
            tcp_cli_disconnect(c, now, "recv_error", err);
        }
    }
}

int tcp_stream_client_run(const struct tcp_stream_client_cfg *cfg, struct linkstats *result)
{
    if (cfg == NULL || cfg->flow == NULL || cfg->rate_mps == 0 || cfg->rate_mps > TCP_MAX_RATE ||
        cfg->payload_len > MSGSTREAM_MAX_PAYLOAD || cfg->max_inflight == 0 ||
        cfg->max_inflight > LAB_TCP_MAX_INFLIGHT_LIMIT || cfg->ack_timeout_ms == 0)
    {
        return LAB_SVC_ERR_CFG;
    }

    struct tcp_cli *c = (struct tcp_cli *)lab_malloc(sizeof(*c));
    if (c == NULL)
    {
        return LAB_SVC_ERR_NOMEM;
    }
    memset(c, 0, sizeof(*c));
    c->cfg = cfg;
    c->fd = -1;
    if (lab_sock_make_addr(cfg->dst_ip, cfg->dst_port, &c->dst) != 0)
    {
        lab_log(LAB_LOG_ERR, "%s: bad destination '%s'", cfg->flow, cfg->dst_ip ? cfg->dst_ip : "");
        lab_free(c);
        return LAB_SVC_ERR_CFG;
    }
    msgstream_parser_init(&c->parser);
    c->session = lab_random_u32();
    c->start_us = lab_time_us();
    c->retry_at_us = c->start_us;
    linkstats_init(&c->st, cfg->flow, c->start_us);
    linkstats_set_first_seq(&c->st, 0);

    char ip[LAB_IPV4_STR_MAX];
    lab_outf("EVENT,%s,t_ms=0,ev=start,dst=%s:%u,rate_mps=%" PRIu32 ",payload=%u,count=%" PRIu32
             ",duration_ms=%" PRIu32 ",session=%08" PRIx32,
             cfg->flow,
             lab_sock_addr_str(&c->dst, ip, sizeof(ip)),
             (unsigned)cfg->dst_port,
             cfg->rate_mps,
             (unsigned)cfg->payload_len,
             cfg->count,
             cfg->duration_ms,
             c->session);

    const uint64_t period_us = 1000000U / cfg->rate_mps;
    const uint64_t report_us = (uint64_t)cfg->report_interval_ms * 1000U;
    const uint64_t ack_timeout_us = (uint64_t)cfg->ack_timeout_ms * 1000U;
    const uint64_t connect_timeout_us =
        (uint64_t)(cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms : 1U) * 1000U;
    uint64_t next_report = c->start_us + report_us;
    bool finishing = false;
    uint64_t finish_deadline = 0;
    uint32_t abandoned_at_finish = 0;
    const char *end_reason = "done";

    for (;;)
    {
        uint64_t now = lab_time_us();

        if (svc_stop_requested(cfg->stop))
        {
            end_reason = "stopped";
            break;
        }
        if (!finishing && ((cfg->count != 0 && c->next_seq >= cfg->count) ||
                           (cfg->duration_ms != 0 &&
                            now - c->start_us >= (uint64_t)cfg->duration_ms * 1000U)))
        {
            finishing = true;
            finish_deadline = now + ack_timeout_us;
            abandoned_at_finish = c->abandoned;
        }
        if (finishing)
        {
            if (c->state != TCP_CLI_UP || tcp_cli_inflight(c) == 0)
            {
                /* The final messages were dropped by a stall, EOF or error during the wait. */
                if (c->abandoned != abandoned_at_finish)
                {
                    end_reason = "disconnected";
                }
                break;
            }
            if (now >= finish_deadline)
            {
                end_reason = "ack_timeout";
                break;
            }
        }

        switch (c->state)
        {
            case TCP_CLI_DOWN:
                if (now >= c->retry_at_us)
                {
                    tcp_cli_start_connect(c, now);
                }
                break;

            case TCP_CLI_CONNECTING:
                if (now - c->connect_started_us >= connect_timeout_us)
                {
                    tcp_cli_connect_failed(c, now, ETIMEDOUT);
                }
                break;

            case TCP_CLI_UP:
                if (tcp_cli_inflight(c) > 0 &&
                    now - c->sent_us[c->una % LAB_TCP_MAX_INFLIGHT_LIMIT] >= ack_timeout_us)
                {
                    c->stalls++;
                    tcp_cli_disconnect(c, now, "stall", 0);
                    break;
                }
                if (!finishing && c->tx_off >= c->tx_len && now >= c->next_send_us &&
                    tcp_cli_inflight(c) < cfg->max_inflight)
                {
                    tcp_cli_queue_msg(c, now);
                    c->next_send_us += period_us;
                    if (now >= c->next_send_us + 1000000U)
                    {
                        c->next_send_us = now + period_us;
                    }
                }
                tcp_cli_flush(c, now);
                break;
        }

        uint32_t timeout_ms = LAB_SVC_POLL_MS;
        int events = 0;
        if (report_us != 0)
        {
            svc_clamp_timeout(&timeout_ms, now, next_report);
        }
        if (finishing)
        {
            svc_clamp_timeout(&timeout_ms, now, finish_deadline);
        }
        switch (c->state)
        {
            case TCP_CLI_DOWN:
                svc_clamp_timeout(&timeout_ms, now, c->retry_at_us);
                break;

            case TCP_CLI_CONNECTING:
                events = LAB_WAIT_WRITE;
                svc_clamp_timeout(&timeout_ms, now, c->connect_started_us + connect_timeout_us);
                break;

            case TCP_CLI_UP:
                events = LAB_WAIT_READ;
                if (c->tx_off < c->tx_len)
                {
                    events |= LAB_WAIT_WRITE;
                }
                else if (!finishing && tcp_cli_inflight(c) < cfg->max_inflight)
                {
                    svc_clamp_timeout(&timeout_ms, now, c->next_send_us);
                }
                if (tcp_cli_inflight(c) > 0)
                {
                    svc_clamp_timeout(&timeout_ms,
                                      now,
                                      c->sent_us[c->una % LAB_TCP_MAX_INFLIGHT_LIMIT] +
                                          ack_timeout_us);
                }
                break;
        }

        int ready = 0;
        if (events == 0)
        {
            if (timeout_ms > 0)
            {
                lab_sleep_ms(timeout_ms);
            }
        }
        else
        {
            ready = lab_sock_wait(c->fd, events, timeout_ms);
        }
        now = lab_time_us();

        if (ready > 0)
        {
            if (c->state == TCP_CLI_CONNECTING && (ready & LAB_WAIT_WRITE))
            {
                tcp_cli_check_connect(c, now);
            }
            else if (c->state == TCP_CLI_UP)
            {
                if (ready & LAB_WAIT_READ)
                {
                    tcp_cli_read(c, now);
                }
                if (ready & LAB_WAIT_WRITE)
                {
                    tcp_cli_flush(c, now);
                }
            }
        }
        else if (ready < 0)
        {
            lab_sleep_ms(1);
        }

        if (report_us != 0 && now >= next_report)
        {
            tcp_cli_report(c, now, false);
            next_report += report_us;
            if (next_report <= now)
            {
                next_report = now + report_us;
            }
        }
    }

    uint64_t now = lab_time_us();
    uint32_t inflight = tcp_cli_inflight(c);
    /* Include what a disconnect during the final wait abandoned, which go_down() cleared. */
    uint32_t unacked = inflight + (finishing ? c->abandoned - abandoned_at_finish : 0);
    lab_sock_close(c->fd);
    c->fd = -1;
    c->state = TCP_CLI_DOWN;
    linkstats_finalize(&c->st, c->next_seq);
    tcp_cli_report(c, now, true);
    lab_outf("EVENT,%s,t_ms=%" PRIu32 ",ev=end,reason=%s,sent=%" PRIu32 ",unacked=%" PRIu32
             ",abandoned=%" PRIu32,
             cfg->flow,
             svc_ms_since(c->start_us, now),
             end_reason,
             c->st.sent,
             unacked,
             c->abandoned + inflight);

    if (result != NULL)
    {
        *result = c->st;
    }
    lab_free(c);
    return LAB_SVC_OK;
}
