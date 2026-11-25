/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "msgstream.h"
#include "test_harness.h"

#define MAX_FRAMES 64
#define STREAM_MAX (MAX_FRAMES * MSGSTREAM_MAX_FRAME + 4096)

static uint8_t stream[STREAM_MAX];
static size_t frame_off[MAX_FRAMES];
static size_t frame_len[MAX_FRAMES];
static struct msgstream_parser parser;

struct sink
{
    uint32_t seqs[MAX_FRAMES * 2];
    size_t n;
    unsigned bad_payload;
};

static void on_frame(void *ctx, const struct linkproto_hdr *hdr, const uint8_t *payload)
{
    struct sink *k = (struct sink *)ctx;
    if (!linkproto_check_pattern(payload, hdr->payload_len, hdr->seq))
    {
        k->bad_payload++;
    }
    if (k->n < sizeof(k->seqs) / sizeof(k->seqs[0]))
    {
        k->seqs[k->n++] = hdr->seq;
    }
}

static size_t append_frame(size_t pos, uint32_t seq, uint16_t plen, enum linkproto_type type)
{
    struct linkproto_hdr h;
    uint8_t payload[MSGSTREAM_MAX_PAYLOAD];
    size_t n = 0;

    linkproto_hdr_init(&h, type, seq, 0x5E55105EUL, 1000ULL * seq);
    h.payload_len = plen;
    linkproto_fill_pattern(payload, plen, seq);
    CHECK_EQ(msgstream_encode(&h, payload, stream + pos, sizeof(stream) - pos, &n), LINKPROTO_OK);
    CHECK_EQ(n, MSGSTREAM_PREFIX_LEN + LINKPROTO_HDR_LEN + plen);
    return n;
}

/* Builds frames with varied payload sizes; returns the stream length. */
static size_t build_stream(size_t nframes)
{
    size_t pos = 0;
    for (size_t i = 0; i < nframes; i++)
    {
        uint16_t plen = (uint16_t)((i * 37U) % 300U);
        if (i == 3)
        {
            plen = MSGSTREAM_MAX_PAYLOAD;
        }
        if (i == 4)
        {
            plen = 0;
        }
        frame_off[i] = pos;
        frame_len[i] = append_frame(pos, (uint32_t)i, plen, LINKPROTO_TYPE_MSG);
        pos += frame_len[i];
    }
    return pos;
}

static void check_sequence(const struct sink *k, size_t nframes, size_t skip)
{
    size_t expect = skip < nframes ? nframes - 1 : nframes;
    CHECK_EQ(k->n, expect);
    size_t j = 0;
    for (size_t i = 0; i < nframes && j < k->n; i++)
    {
        if (i == skip)
        {
            continue;
        }
        CHECK_EQ(k->seqs[j], i);
        j++;
    }
    CHECK_EQ(k->bad_payload, 0);
}

static void test_whole_stream(void)
{
    struct sink k = { 0 };
    size_t len = build_stream(20);

    msgstream_parser_init(&parser);
    CHECK_EQ(msgstream_parser_feed(&parser, stream, len, on_frame, &k), 20);
    check_sequence(&k, 20, SIZE_MAX);
    CHECK_EQ(parser.fill, 0);
    CHECK_EQ(parser.stats.frames, 20);
    CHECK_EQ(parser.stats.bytes_in, len);
    CHECK_EQ(parser.stats.resyncs, 0);
    CHECK_EQ(parser.stats.bytes_discarded, 0);
}

static void test_one_byte_chunks(void)
{
    struct sink k = { 0 };
    size_t len = build_stream(20);

    msgstream_parser_init(&parser);
    for (size_t i = 0; i < len; i++)
    {
        (void)msgstream_parser_feed(&parser, stream + i, 1, on_frame, &k);
    }
    check_sequence(&k, 20, SIZE_MAX);
    CHECK_EQ(parser.stats.resyncs, 0);
}

static void test_random_splits(void)
{
    size_t len = build_stream(MAX_FRAMES);

    for (int iter = 0; iter < 200; iter++)
    {
        struct sink k = { 0 };
        msgstream_parser_init(&parser);
        size_t pos = 0;
        while (pos < len)
        {
            size_t chunk = 1 + test_rand() % (iter % 2 ? 7U : 3000U);
            if (chunk > len - pos)
            {
                chunk = len - pos;
            }
            (void)msgstream_parser_feed(&parser, stream + pos, chunk, on_frame, &k);
            pos += chunk;
        }
        check_sequence(&k, MAX_FRAMES, SIZE_MAX);
        CHECK_EQ(parser.stats.resyncs, 0);
    }
}

/* Flip each byte of one frame in turn: exactly that frame is lost, the rest survive. */
static void test_corruption_every_byte(void)
{
    const size_t nframes = 6;
    const size_t victim = 2;
    size_t len = build_stream(nframes);

    for (size_t off = 0; off < frame_len[victim]; off++)
    {
        for (int pass = 0; pass < 2; pass++)
        {
            struct sink k = { 0 };
            uint8_t mask = pass == 0 ? 0xFF : (uint8_t)(1U << (off % 8));
            stream[frame_off[victim] + off] ^= mask;

            msgstream_parser_init(&parser);
            size_t pos = 0;
            while (pos < len)
            {
                size_t chunk = 1 + test_rand() % 64U;
                if (chunk > len - pos)
                {
                    chunk = len - pos;
                }
                (void)msgstream_parser_feed(&parser, stream + pos, chunk, on_frame, &k);
                pos += chunk;
            }
            check_sequence(&k, nframes, victim);
            CHECK_EQ(parser.stats.resyncs, 1);
            CHECK(parser.stats.bytes_discarded >= 1);
            CHECK(parser.stats.len_errors + parser.stats.hdr_errors + parser.stats.crc_errors >= 1);

            stream[frame_off[victim] + off] ^= mask;
        }
    }
}

static void test_garbage_prefix_and_between(void)
{
    struct sink k = { 0 };
    size_t pos = 0;

    /* Junk including a fake magic that is not followed by a valid header. */
    static const uint8_t junk1[] = { 0x00, 0x20, 'H', 'L', 'R', 'L', 0x09, 0x09, 0xAA, 0xBB };
    memcpy(stream, junk1, sizeof(junk1));
    pos += sizeof(junk1);
    pos += append_frame(pos, 0, 10, LINKPROTO_TYPE_MSG);
    for (int i = 0; i < 100; i++)
    {
        stream[pos++] = (uint8_t)test_rand();
    }
    pos += append_frame(pos, 1, 0, LINKPROTO_TYPE_MSG_ACK);
    pos += append_frame(pos, 2, 500, LINKPROTO_TYPE_MSG);

    msgstream_parser_init(&parser);
    (void)msgstream_parser_feed(&parser, stream, pos, on_frame, &k);
    check_sequence(&k, 3, SIZE_MAX);
    CHECK_EQ(parser.stats.resyncs, 2);
    CHECK_EQ(parser.stats.bytes_discarded, sizeof(junk1) + 100);
}

static void test_truncated_then_reset(void)
{
    struct sink k = { 0 };
    size_t len = build_stream(3);

    msgstream_parser_init(&parser);
    (void)msgstream_parser_feed(&parser, stream, len - 5, on_frame, &k);
    CHECK_EQ(k.n, 2);
    CHECK(parser.fill > 0);

    /* Connection dropped: the partial frame is discarded, a new stream parses cleanly. */
    msgstream_parser_reset(&parser);
    CHECK_EQ(parser.fill, 0);
    (void)msgstream_parser_feed(&parser, stream, len, on_frame, &k);
    CHECK_EQ(k.n, 5);
    CHECK_EQ(parser.stats.resyncs, 0);
    CHECK_EQ(parser.stats.frames, 5);
}

static void test_length_limits(void)
{
    struct sink k = { 0 };
    uint8_t frame[MSGSTREAM_MAX_FRAME + 8];
    struct linkproto_hdr h;
    size_t n = 0;

    /* Payload above the stream maximum cannot be encoded. */
    linkproto_hdr_init(&h, LINKPROTO_TYPE_MSG, 1, 1, 1);
    h.payload_len = MSGSTREAM_MAX_PAYLOAD + 1;
    CHECK_EQ(msgstream_encode(&h, NULL, frame, sizeof(frame), &n), LINKPROTO_ERR_LEN);
    h.payload_len = 10;
    CHECK_EQ(msgstream_encode(&h, NULL, frame, MSGSTREAM_PREFIX_LEN + LINKPROTO_HDR_LEN + 9, &n),
             LINKPROTO_ERR_NOSPACE);
    CHECK_EQ(msgstream_encode(&h, NULL, frame, 1, &n), LINKPROTO_ERR_NOSPACE);
    CHECK_EQ(msgstream_encode(NULL, NULL, frame, sizeof(frame), &n), LINKPROTO_ERR_ARG);

    /* Length prefixes outside [HDR_LEN, MAX_PACKET] are rejected immediately. */
    msgstream_parser_init(&parser);
    static const uint8_t too_small[] = { 0x00, LINKPROTO_HDR_LEN - 1 };
    (void)msgstream_parser_feed(&parser, too_small, sizeof(too_small), on_frame, &k);
    CHECK_EQ(parser.stats.len_errors, 1);
    uint8_t too_big[2];
    linkproto_put_be16(too_big, MSGSTREAM_MAX_PACKET + 1);
    msgstream_parser_init(&parser);
    (void)msgstream_parser_feed(&parser, too_big, sizeof(too_big), on_frame, &k);
    CHECK_EQ(parser.stats.len_errors, 1);
    CHECK_EQ(k.n, 0);

    /* A valid linkproto packet that is too large for the stream (LINKPROTO_MAX_PAYLOAD). */
#if LINKPROTO_MAX_PAYLOAD > MSGSTREAM_MAX_PAYLOAD
    static uint8_t big[MSGSTREAM_PREFIX_LEN + LINKPROTO_MAX_PACKET];
    h.payload_len = MSGSTREAM_MAX_PAYLOAD + 1;
    CHECK_EQ(linkproto_encode(&h, NULL, big + 2, sizeof(big) - 2, &n), LINKPROTO_OK);
    linkproto_put_be16(big, (uint16_t)n);
    msgstream_parser_init(&parser);
    (void)msgstream_parser_feed(&parser, big, MSGSTREAM_PREFIX_LEN + n, on_frame, &k);
    CHECK_EQ(k.n, 0);
    CHECK(parser.stats.len_errors >= 1);
#endif

    /* Maximum-size frame fed in one go into an otherwise empty parser. */
    h.payload_len = MSGSTREAM_MAX_PAYLOAD;
    linkproto_fill_pattern(frame + MSGSTREAM_PREFIX_LEN + LINKPROTO_HDR_LEN, MSGSTREAM_MAX_PAYLOAD, 1);
    CHECK_EQ(msgstream_encode(&h, NULL, frame, sizeof(frame), &n), LINKPROTO_OK);
    CHECK_EQ(n, MSGSTREAM_MAX_FRAME);
    msgstream_parser_init(&parser);
    CHECK_EQ(msgstream_parser_feed(&parser, frame, n, on_frame, &k), 1);
    CHECK_EQ(k.bad_payload, 0);
}

static void test_no_callback(void)
{
    size_t len = build_stream(5);
    msgstream_parser_init(&parser);
    CHECK_EQ(msgstream_parser_feed(&parser, stream, len, NULL, NULL), 5);
}

static const struct test_case cases[] = {
    { "msgstream/whole_stream", test_whole_stream },
    { "msgstream/one_byte_chunks", test_one_byte_chunks },
    { "msgstream/random_splits", test_random_splits },
    { "msgstream/corruption_every_byte", test_corruption_every_byte },
    { "msgstream/garbage_prefix_and_between", test_garbage_prefix_and_between },
    { "msgstream/truncated_then_reset", test_truncated_then_reset },
    { "msgstream/length_limits", test_length_limits },
    { "msgstream/no_callback", test_no_callback },
};

void test_msgstream_register(const struct test_case **out, size_t *n)
{
    *out = cases;
    *n = sizeof(cases) / sizeof(cases[0]);
}
