/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "msgstream.h"

static const uint8_t magic_bytes[4] = { 0x48, 0x4C, 0x52, 0x4C };

void msgstream_parser_init(struct msgstream_parser *p)
{
    memset(p, 0, sizeof(*p));
}

void msgstream_parser_reset(struct msgstream_parser *p)
{
    p->fill = 0;
    p->in_resync = false;
}

static void consume(struct msgstream_parser *p, size_t n)
{
    if (n >= p->fill)
    {
        p->fill = 0;
        return;
    }
    memmove(p->buf, p->buf + n, p->fill - n);
    p->fill -= n;
}

/* True if a frame starting at @p off could be valid as far as its magic bytes are buffered. */
static bool magic_plausible(const struct msgstream_parser *p, size_t off)
{
    size_t start = off + MSGSTREAM_PREFIX_LEN;
    if (start >= p->fill)
    {
        return true;
    }
    size_t n = p->fill - start;
    if (n > sizeof(magic_bytes))
    {
        n = sizeof(magic_bytes);
    }
    return memcmp(p->buf + start, magic_bytes, n) == 0;
}

/* Drop at least one byte, up to the next plausible frame start. */
static void resync(struct msgstream_parser *p)
{
    if (!p->in_resync)
    {
        p->in_resync = true;
        p->stats.resyncs++;
    }

    size_t skip = 1;
    while (skip < p->fill && !magic_plausible(p, skip))
    {
        skip++;
    }
    p->stats.bytes_discarded += (uint32_t)skip;
    consume(p, skip);
}

static size_t process(struct msgstream_parser *p, msgstream_frame_cb_t cb, void *ctx)
{
    size_t delivered = 0;

    while (p->fill >= MSGSTREAM_PREFIX_LEN)
    {
        size_t pkt_len = linkproto_get_be16(p->buf);
        if (pkt_len < LINKPROTO_HDR_LEN || pkt_len > MSGSTREAM_MAX_PACKET)
        {
            p->stats.len_errors++;
            resync(p);
            continue;
        }
        if (!magic_plausible(p, 0))
        {
            p->stats.hdr_errors++;
            resync(p);
            continue;
        }
        if (p->fill < MSGSTREAM_PREFIX_LEN + LINKPROTO_HDR_LEN)
        {
            break;
        }

        const uint8_t *pkt = p->buf + MSGSTREAM_PREFIX_LEN;
        if (linkproto_check_header(pkt, LINKPROTO_HDR_LEN, pkt_len, NULL) != LINKPROTO_OK)
        {
            p->stats.hdr_errors++;
            resync(p);
            continue;
        }
        if (p->fill < MSGSTREAM_PREFIX_LEN + pkt_len)
        {
            break;
        }

        struct linkproto_hdr hdr;
        const uint8_t *payload = NULL;
        if (linkproto_decode(pkt, pkt_len, &hdr, &payload) != LINKPROTO_OK)
        {
            p->stats.crc_errors++;
            resync(p);
            continue;
        }

        p->in_resync = false;
        p->stats.frames++;
        delivered++;
        if (cb != NULL)
        {
            cb(ctx, &hdr, payload);
        }
        consume(p, MSGSTREAM_PREFIX_LEN + pkt_len);
    }
    return delivered;
}

size_t msgstream_parser_feed(struct msgstream_parser *p,
                             const uint8_t *data,
                             size_t len,
                             msgstream_frame_cb_t cb,
                             void *ctx)
{
    size_t delivered = 0;

    while (len > 0)
    {
        /* process() always leaves room: any incomplete frame is < MSGSTREAM_MAX_FRAME. */
        size_t room = sizeof(p->buf) - p->fill;
        size_t n = len < room ? len : room;

        memcpy(p->buf + p->fill, data, n);
        p->fill += n;
        p->stats.bytes_in += (uint32_t)n;
        data += n;
        len -= n;
        delivered += process(p, cb, ctx);
    }
    return delivered;
}

enum linkproto_result msgstream_encode(const struct linkproto_hdr *hdr,
                                       const uint8_t *payload,
                                       uint8_t *buf,
                                       size_t buf_len,
                                       size_t *out_len)
{
    if (hdr == NULL || buf == NULL)
    {
        return LINKPROTO_ERR_ARG;
    }
    if (hdr->payload_len > MSGSTREAM_MAX_PAYLOAD)
    {
        return LINKPROTO_ERR_LEN;
    }
    if (buf_len < MSGSTREAM_PREFIX_LEN)
    {
        return LINKPROTO_ERR_NOSPACE;
    }

    size_t pkt_len = 0;
    enum linkproto_result r = linkproto_encode(hdr,
                                               payload,
                                               buf + MSGSTREAM_PREFIX_LEN,
                                               buf_len - MSGSTREAM_PREFIX_LEN,
                                               &pkt_len);
    if (r != LINKPROTO_OK)
    {
        return r;
    }
    linkproto_put_be16(buf, (uint16_t)pkt_len);
    if (out_len != NULL)
    {
        *out_len = MSGSTREAM_PREFIX_LEN + pkt_len;
    }
    return LINKPROTO_OK;
}
