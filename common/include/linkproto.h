/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Lab wire format ("HLRL"), shared by UDP echo and the TCP message stream.
 *
 * All multi-byte fields are big-endian (network byte order). Packets are produced and parsed
 * only through the explicit encode/decode functions below; never cast a buffer to a struct.
 *
 * | Offset | Size | Field       | Notes                                                    |
 * |--------|------|-------------|----------------------------------------------------------|
 * | 0      | 4    | magic       | 0x484C524C ("HLRL")                                      |
 * | 4      | 1    | version     | @ref LINKPROTO_VERSION                                   |
 * | 5      | 1    | type        | @ref linkproto_type                                      |
 * | 6      | 1    | flags       | @ref LINKPROTO_FLAG_LAST; other bits must be 0           |
 * | 7      | 1    | reserved    | must be 0                                                |
 * | 8      | 4    | seq         | per-session sequence number (wraps at 2^32)              |
 * | 12     | 4    | session     | random per client run; echoed back                       |
 * | 16     | 8    | ts_us       | origin timestamp, sender's monotonic clock; echoed back  |
 * | 24     | 2    | status      | 0 in requests; @ref linkproto_status in responses        |
 * | 26     | 2    | payload_len | <= @ref LINKPROTO_MAX_PAYLOAD                            |
 * | 28     | 4    | crc32       | IEEE CRC-32 (zlib) over bytes 0..27 then the payload     |
 * | 32     | n    | payload     |                                                          |
 */

#ifndef LINKPROTO_H
#define LINKPROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define LINKPROTO_MAGIC   0x484C524CUL
#define LINKPROTO_VERSION 1
#define LINKPROTO_HDR_LEN 32

/** Largest payload: 32 + 1400 + UDP/IPv4 headers stays below a 1500-byte MTU. */
#ifndef LINKPROTO_MAX_PAYLOAD
#define LINKPROTO_MAX_PAYLOAD 1400
#endif

#define LINKPROTO_MAX_PACKET (LINKPROTO_HDR_LEN + LINKPROTO_MAX_PAYLOAD)

/** Sender's final packet of a run (receiver may emit final statistics). */
#define LINKPROTO_FLAG_LAST  0x01
#define LINKPROTO_FLAGS_MASK (LINKPROTO_FLAG_LAST)

enum linkproto_type
{
    LINKPROTO_TYPE_ECHO_REQ = 1,
    LINKPROTO_TYPE_ECHO_RSP = 2,
    LINKPROTO_TYPE_MSG = 3,
    LINKPROTO_TYPE_MSG_ACK = 4,
};

enum linkproto_status
{
    LINKPROTO_STATUS_OK = 0,
    /** Receiver found a payload that does not match the expected test pattern. */
    LINKPROTO_STATUS_BAD_PAYLOAD = 1,
};

enum linkproto_result
{
    LINKPROTO_OK = 0,
    /** NULL pointer or otherwise invalid argument. */
    LINKPROTO_ERR_ARG,
    /** Fewer bytes than a header. */
    LINKPROTO_ERR_SHORT,
    LINKPROTO_ERR_MAGIC,
    LINKPROTO_ERR_VERSION,
    LINKPROTO_ERR_TYPE,
    /** Undefined flag bits set. */
    LINKPROTO_ERR_FLAGS,
    /** Reserved byte non-zero, or non-zero status in a request type. */
    LINKPROTO_ERR_RESERVED,
    /** payload_len exceeds LINKPROTO_MAX_PAYLOAD. */
    LINKPROTO_ERR_LEN,
    /** Buffer length differs from header + payload_len (truncated or trailing bytes). */
    LINKPROTO_ERR_TRUNC,
    LINKPROTO_ERR_CRC,
    /** Output buffer too small (encode only). */
    LINKPROTO_ERR_NOSPACE,
};

/** Decoded header (host byte order). Magic and CRC are handled by the codec. */
struct linkproto_hdr
{
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint32_t seq;
    uint32_t session;
    uint64_t ts_us;
    uint16_t status;
    uint16_t payload_len;
};

/** Short, stable name for a result code (e.g. "crc"). */
const char *linkproto_result_str(enum linkproto_result r);

/**
 * IEEE 802.3 CRC-32, identical to zlib crc32(): start with @p crc = 0 and chain calls.
 */
uint32_t linkproto_crc32(uint32_t crc, const void *data, size_t len);

/**
 * Fill @p hdr with version, type and the given fields; flags/status/payload_len zero.
 */
void linkproto_hdr_init(struct linkproto_hdr *hdr,
                        enum linkproto_type type,
                        uint32_t seq,
                        uint32_t session,
                        uint64_t ts_us);

/**
 * Encode a packet. @p hdr->payload_len gives the payload size; @p payload may be NULL when
 * the payload is already in place at @p buf + LINKPROTO_HDR_LEN (or payload_len is 0), and
 * may overlap @p buf.
 *
 * @param out_len   Set to the encoded length on success (may be NULL).
 */
enum linkproto_result linkproto_encode(const struct linkproto_hdr *hdr,
                                       const uint8_t *payload,
                                       uint8_t *buf,
                                       size_t buf_len,
                                       size_t *out_len);

/**
 * Validate the header fields of a packet whose total length will be @p pkt_len, without
 * checking the CRC. Requires @p avail >= LINKPROTO_HDR_LEN bytes at @p buf. Used to reject a
 * corrupted stream frame before its payload has arrived.
 */
enum linkproto_result linkproto_check_header(const uint8_t *buf,
                                             size_t avail,
                                             size_t pkt_len,
                                             struct linkproto_hdr *hdr);

/**
 * Strictly decode exactly @p len bytes (magic, version, type, flags, reserved, length, CRC).
 *
 * @param payload   Set to the payload inside @p buf (may be NULL).
 */
enum linkproto_result linkproto_decode(const uint8_t *buf,
                                       size_t len,
                                       struct linkproto_hdr *hdr,
                                       const uint8_t **payload);

/** Deterministic test pattern for a payload of @p len bytes belonging to @p seq. */
void linkproto_fill_pattern(uint8_t *payload, size_t len, uint32_t seq);

/** True if @p payload matches linkproto_fill_pattern(seq). */
bool linkproto_check_pattern(const uint8_t *payload, size_t len, uint32_t seq);

static inline void linkproto_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void linkproto_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void linkproto_put_be64(uint8_t *p, uint64_t v)
{
    linkproto_put_be32(p, (uint32_t)(v >> 32));
    linkproto_put_be32(p + 4, (uint32_t)v);
}

static inline uint16_t linkproto_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t linkproto_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint64_t linkproto_get_be64(const uint8_t *p)
{
    return ((uint64_t)linkproto_get_be32(p) << 32) | linkproto_get_be32(p + 4);
}

#ifdef __cplusplus
}
#endif

#endif /* LINKPROTO_H */
