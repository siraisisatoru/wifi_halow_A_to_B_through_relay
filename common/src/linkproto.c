/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "linkproto.h"

#define OFF_MAGIC       0
#define OFF_VERSION     4
#define OFF_TYPE        5
#define OFF_FLAGS       6
#define OFF_RESERVED    7
#define OFF_SEQ         8
#define OFF_SESSION     12
#define OFF_TS          16
#define OFF_STATUS      24
#define OFF_PAYLOAD_LEN 26
#define OFF_CRC         28

/* Nibble-wise table for the reflected polynomial 0xEDB88320: small enough for any target. */
static const uint32_t crc32_nibble_table[16] = {
    0x00000000UL,
    0x1DB71064UL,
    0x3B6E20C8UL,
    0x26D930ACUL,
    0x76DC4190UL,
    0x6B6B51F4UL,
    0x4DB26158UL,
    0x5005713CUL,
    0xEDB88320UL,
    0xF00F9344UL,
    0xD6D6A3E8UL,
    0xCB61B38CUL,
    0x9B64C2B0UL,
    0x86D3D2D4UL,
    0xA00AE278UL,
    0xBDBDF21CUL,
};

const char *linkproto_result_str(enum linkproto_result r)
{
    switch (r)
    {
        case LINKPROTO_OK:
            return "ok";

        case LINKPROTO_ERR_ARG:
            return "arg";

        case LINKPROTO_ERR_SHORT:
            return "short";

        case LINKPROTO_ERR_MAGIC:
            return "magic";

        case LINKPROTO_ERR_VERSION:
            return "version";

        case LINKPROTO_ERR_TYPE:
            return "type";

        case LINKPROTO_ERR_FLAGS:
            return "flags";

        case LINKPROTO_ERR_RESERVED:
            return "reserved";

        case LINKPROTO_ERR_LEN:
            return "len";

        case LINKPROTO_ERR_TRUNC:
            return "trunc";

        case LINKPROTO_ERR_CRC:
            return "crc";

        case LINKPROTO_ERR_NOSPACE:
            return "nospace";
    }
    return "unknown";
}

uint32_t linkproto_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFUL;

    while (len-- > 0)
    {
        c ^= *p++;
        c = (c >> 4) ^ crc32_nibble_table[c & 0x0F];
        c = (c >> 4) ^ crc32_nibble_table[c & 0x0F];
    }
    return c ^ 0xFFFFFFFFUL;
}

void linkproto_hdr_init(struct linkproto_hdr *hdr,
                        enum linkproto_type type,
                        uint32_t seq,
                        uint32_t session,
                        uint64_t ts_us)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->version = LINKPROTO_VERSION;
    hdr->type = (uint8_t)type;
    hdr->seq = seq;
    hdr->session = session;
    hdr->ts_us = ts_us;
}

static bool type_is_valid(uint8_t type)
{
    return type >= LINKPROTO_TYPE_ECHO_REQ && type <= LINKPROTO_TYPE_MSG_ACK;
}

static bool type_is_request(uint8_t type)
{
    return type == LINKPROTO_TYPE_ECHO_REQ || type == LINKPROTO_TYPE_MSG;
}

enum linkproto_result linkproto_encode(const struct linkproto_hdr *hdr,
                                       const uint8_t *payload,
                                       uint8_t *buf,
                                       size_t buf_len,
                                       size_t *out_len)
{
    if (hdr == NULL || buf == NULL)
    {
        return LINKPROTO_ERR_ARG;
    }
    if (hdr->version != LINKPROTO_VERSION)
    {
        return LINKPROTO_ERR_VERSION;
    }
    if (!type_is_valid(hdr->type))
    {
        return LINKPROTO_ERR_TYPE;
    }
    if ((hdr->flags & (uint8_t)~LINKPROTO_FLAGS_MASK) != 0)
    {
        return LINKPROTO_ERR_FLAGS;
    }
    if (type_is_request(hdr->type) && hdr->status != 0)
    {
        return LINKPROTO_ERR_RESERVED;
    }
    if (hdr->payload_len > LINKPROTO_MAX_PAYLOAD)
    {
        return LINKPROTO_ERR_LEN;
    }

    size_t total = (size_t)LINKPROTO_HDR_LEN + hdr->payload_len;
    if (buf_len < total)
    {
        return LINKPROTO_ERR_NOSPACE;
    }

    if (payload != NULL && hdr->payload_len > 0 && payload != buf + LINKPROTO_HDR_LEN)
    {
        memmove(buf + LINKPROTO_HDR_LEN, payload, hdr->payload_len);
    }

    linkproto_put_be32(buf + OFF_MAGIC, LINKPROTO_MAGIC);
    buf[OFF_VERSION] = hdr->version;
    buf[OFF_TYPE] = hdr->type;
    buf[OFF_FLAGS] = hdr->flags;
    buf[OFF_RESERVED] = 0;
    linkproto_put_be32(buf + OFF_SEQ, hdr->seq);
    linkproto_put_be32(buf + OFF_SESSION, hdr->session);
    linkproto_put_be64(buf + OFF_TS, hdr->ts_us);
    linkproto_put_be16(buf + OFF_STATUS, hdr->status);
    linkproto_put_be16(buf + OFF_PAYLOAD_LEN, hdr->payload_len);

    uint32_t crc = linkproto_crc32(0, buf, OFF_CRC);
    crc = linkproto_crc32(crc, buf + LINKPROTO_HDR_LEN, hdr->payload_len);
    linkproto_put_be32(buf + OFF_CRC, crc);

    if (out_len != NULL)
    {
        *out_len = total;
    }
    return LINKPROTO_OK;
}

enum linkproto_result linkproto_check_header(const uint8_t *buf,
                                             size_t avail,
                                             size_t pkt_len,
                                             struct linkproto_hdr *hdr)
{
    if (buf == NULL)
    {
        return LINKPROTO_ERR_ARG;
    }
    if (avail < LINKPROTO_HDR_LEN || pkt_len < LINKPROTO_HDR_LEN)
    {
        return LINKPROTO_ERR_SHORT;
    }
    if (linkproto_get_be32(buf + OFF_MAGIC) != LINKPROTO_MAGIC)
    {
        return LINKPROTO_ERR_MAGIC;
    }
    if (buf[OFF_VERSION] != LINKPROTO_VERSION)
    {
        return LINKPROTO_ERR_VERSION;
    }
    if (!type_is_valid(buf[OFF_TYPE]))
    {
        return LINKPROTO_ERR_TYPE;
    }
    if ((buf[OFF_FLAGS] & (uint8_t)~LINKPROTO_FLAGS_MASK) != 0)
    {
        return LINKPROTO_ERR_FLAGS;
    }

    uint16_t status = linkproto_get_be16(buf + OFF_STATUS);
    if (buf[OFF_RESERVED] != 0 || (type_is_request(buf[OFF_TYPE]) && status != 0))
    {
        return LINKPROTO_ERR_RESERVED;
    }

    uint16_t payload_len = linkproto_get_be16(buf + OFF_PAYLOAD_LEN);
    if (payload_len > LINKPROTO_MAX_PAYLOAD)
    {
        return LINKPROTO_ERR_LEN;
    }
    if (pkt_len != (size_t)LINKPROTO_HDR_LEN + payload_len)
    {
        return LINKPROTO_ERR_TRUNC;
    }

    if (hdr != NULL)
    {
        hdr->version = buf[OFF_VERSION];
        hdr->type = buf[OFF_TYPE];
        hdr->flags = buf[OFF_FLAGS];
        hdr->seq = linkproto_get_be32(buf + OFF_SEQ);
        hdr->session = linkproto_get_be32(buf + OFF_SESSION);
        hdr->ts_us = linkproto_get_be64(buf + OFF_TS);
        hdr->status = status;
        hdr->payload_len = payload_len;
    }
    return LINKPROTO_OK;
}

enum linkproto_result linkproto_decode(const uint8_t *buf,
                                       size_t len,
                                       struct linkproto_hdr *hdr,
                                       const uint8_t **payload)
{
    struct linkproto_hdr tmp;

    enum linkproto_result r = linkproto_check_header(buf, len, len, &tmp);
    if (r != LINKPROTO_OK)
    {
        return r;
    }

    uint32_t crc = linkproto_crc32(0, buf, OFF_CRC);
    crc = linkproto_crc32(crc, buf + LINKPROTO_HDR_LEN, tmp.payload_len);
    if (crc != linkproto_get_be32(buf + OFF_CRC))
    {
        return LINKPROTO_ERR_CRC;
    }

    if (hdr != NULL)
    {
        *hdr = tmp;
    }
    if (payload != NULL)
    {
        *payload = buf + LINKPROTO_HDR_LEN;
    }
    return LINKPROTO_OK;
}

static uint8_t pattern_byte(uint32_t key, size_t i)
{
    return (uint8_t)((key >> ((i & 3U) * 8U)) + (uint32_t)i);
}

void linkproto_fill_pattern(uint8_t *payload, size_t len, uint32_t seq)
{
    uint32_t key = seq * 2654435761UL;

    for (size_t i = 0; i < len; i++)
    {
        payload[i] = pattern_byte(key, i);
    }
}

bool linkproto_check_pattern(const uint8_t *payload, size_t len, uint32_t seq)
{
    uint32_t key = seq * 2654435761UL;

    for (size_t i = 0; i < len; i++)
    {
        if (payload[i] != pattern_byte(key, i))
        {
            return false;
        }
    }
    return true;
}
