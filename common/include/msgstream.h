/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Framing of linkproto packets over a TCP byte stream.
 *
 * Frame = 2-byte big-endian length L, then exactly L bytes of one linkproto packet
 * (LINKPROTO_HDR_LEN <= L <= MSGSTREAM_MAX_PACKET).
 *
 * The parser accepts arbitrary chunking. On any inconsistency (length out of range, bad
 * magic/header, CRC mismatch) it drops bytes until a position where a length prefix is
 * followed by the "HLRL" magic, so a corrupted frame costs at most that frame.
 */

#ifndef MSGSTREAM_H
#define MSGSTREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "linkproto.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MSGSTREAM_PREFIX_LEN 2

/** Largest stream message payload (also bounds the parser buffer). */
#ifndef MSGSTREAM_MAX_PAYLOAD
#define MSGSTREAM_MAX_PAYLOAD 1024
#endif

#if MSGSTREAM_MAX_PAYLOAD > LINKPROTO_MAX_PAYLOAD
#error "MSGSTREAM_MAX_PAYLOAD must not exceed LINKPROTO_MAX_PAYLOAD"
#endif

#define MSGSTREAM_MAX_PACKET (LINKPROTO_HDR_LEN + MSGSTREAM_MAX_PAYLOAD)
#define MSGSTREAM_MAX_FRAME  (MSGSTREAM_PREFIX_LEN + MSGSTREAM_MAX_PACKET)
#define MSGSTREAM_MIN_FRAME  (MSGSTREAM_PREFIX_LEN + LINKPROTO_HDR_LEN)

struct msgstream_stats
{
    /** Valid frames delivered. */
    uint32_t frames;
    uint32_t bytes_in;
    /** Bytes dropped while resynchronising. */
    uint32_t bytes_discarded;
    /** Number of times the parser lost sync (one per corruption episode). */
    uint32_t resyncs;
    /** Length prefix out of range. */
    uint32_t len_errors;
    /** Header invalid (magic/version/type/flags/reserved/length mismatch). */
    uint32_t hdr_errors;
    uint32_t crc_errors;
};

/**
 * Called for each valid frame. @p payload points into the parser buffer and is valid only
 * during the call.
 */
typedef void (*msgstream_frame_cb_t)(void *ctx,
                                     const struct linkproto_hdr *hdr,
                                     const uint8_t *payload);

struct msgstream_parser
{
    uint8_t buf[MSGSTREAM_MAX_FRAME];
    size_t fill;
    bool in_resync;
    struct msgstream_stats stats;
};

/** Initialise (clears stats). */
void msgstream_parser_init(struct msgstream_parser *p);

/** Drop buffered bytes (e.g. on reconnect); keeps stats. */
void msgstream_parser_reset(struct msgstream_parser *p);

/**
 * Consume @p len bytes, calling @p cb for every complete valid frame.
 *
 * @returns the number of frames delivered by this call.
 */
size_t msgstream_parser_feed(struct msgstream_parser *p,
                             const uint8_t *data,
                             size_t len,
                             msgstream_frame_cb_t cb,
                             void *ctx);

/**
 * Encode one frame (length prefix + linkproto packet) into @p buf. @p payload may be NULL when
 * already in place at @p buf + MSGSTREAM_PREFIX_LEN + LINKPROTO_HDR_LEN.
 */
enum linkproto_result msgstream_encode(const struct linkproto_hdr *hdr,
                                       const uint8_t *payload,
                                       uint8_t *buf,
                                       size_t buf_len,
                                       size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MSGSTREAM_H */
