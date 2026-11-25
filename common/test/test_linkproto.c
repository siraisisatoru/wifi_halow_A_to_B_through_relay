/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "linkproto.h"
#include "test_harness.h"

static uint8_t buf[LINKPROTO_MAX_PACKET + 64];
static uint8_t payload[LINKPROTO_MAX_PAYLOAD + 1];

static size_t make_packet(enum linkproto_type type, uint16_t plen, uint32_t seq)
{
    struct linkproto_hdr h;
    size_t len = 0;

    linkproto_hdr_init(&h, type, seq, 0xA1B2C3D4UL, 0x0102030405060708ULL);
    h.payload_len = plen;
    linkproto_fill_pattern(payload, plen, seq);
    CHECK_EQ(linkproto_encode(&h, payload, buf, sizeof(buf), &len), LINKPROTO_OK);
    CHECK_EQ(len, LINKPROTO_HDR_LEN + plen);
    return len;
}

/* Re-sign a hand-modified header so only the field under test is wrong. */
static void fix_crc(size_t len)
{
    uint32_t crc = linkproto_crc32(0, buf, 28);
    crc = linkproto_crc32(crc, buf + LINKPROTO_HDR_LEN, len - LINKPROTO_HDR_LEN);
    linkproto_put_be32(buf + 28, crc);
}

static void test_crc32_vectors(void)
{
    CHECK_EQ(linkproto_crc32(0, "123456789", 9), 0xCBF43926UL);
    CHECK_EQ(linkproto_crc32(0, "", 0), 0);
    CHECK_EQ(linkproto_crc32(0, "The quick brown fox jumps over the lazy dog", 43), 0x414FA339UL);

    /* Chaining equals one pass. */
    uint32_t c = linkproto_crc32(0, "12345", 5);
    c = linkproto_crc32(c, "6789", 4);
    CHECK_EQ(c, 0xCBF43926UL);
}

static void test_roundtrip_sizes(void)
{
    static const uint16_t sizes[] = { 0, 1, 2, 3, 31, 32, 64, 255, 256, 1023, LINKPROTO_MAX_PAYLOAD };
    static const enum linkproto_type types[] = {
        LINKPROTO_TYPE_ECHO_REQ,
        LINKPROTO_TYPE_ECHO_RSP,
        LINKPROTO_TYPE_MSG,
        LINKPROTO_TYPE_MSG_ACK,
    };

    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++)
    {
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        {
            struct linkproto_hdr h;
            linkproto_hdr_init(&h, types[t], 0xFFFFFFFFUL - (uint32_t)i, 0xDEADBEEFUL,
                               0xFEDCBA9876543210ULL + i);
            h.payload_len = sizes[i];
            h.flags = LINKPROTO_FLAG_LAST;
            if (types[t] == LINKPROTO_TYPE_ECHO_RSP || types[t] == LINKPROTO_TYPE_MSG_ACK)
            {
                h.status = LINKPROTO_STATUS_BAD_PAYLOAD;
            }
            linkproto_fill_pattern(payload, sizes[i], h.seq);

            size_t len = 0;
            CHECK_EQ(linkproto_encode(&h, payload, buf, sizeof(buf), &len), LINKPROTO_OK);
            CHECK_EQ(len, LINKPROTO_HDR_LEN + sizes[i]);

            struct linkproto_hdr d;
            const uint8_t *p = NULL;
            CHECK_EQ(linkproto_decode(buf, len, &d, &p), LINKPROTO_OK);
            CHECK_EQ(d.version, LINKPROTO_VERSION);
            CHECK_EQ(d.type, types[t]);
            CHECK_EQ(d.flags, LINKPROTO_FLAG_LAST);
            CHECK_EQ(d.seq, h.seq);
            CHECK_EQ(d.session, 0xDEADBEEFUL);
            CHECK_EQ(d.ts_us, h.ts_us);
            CHECK_EQ(d.status, h.status);
            CHECK_EQ(d.payload_len, sizes[i]);
            CHECK(p == buf + LINKPROTO_HDR_LEN);
            CHECK(linkproto_check_pattern(p, d.payload_len, d.seq));
        }
    }
}

static void test_wire_layout(void)
{
    struct linkproto_hdr h;
    linkproto_hdr_init(&h, LINKPROTO_TYPE_MSG, 0x11223344UL, 0x55667788UL, 0x99AABBCCDDEEFF00ULL);
    h.payload_len = 3;
    const uint8_t pl[3] = { 0xAB, 0xCD, 0xEF };
    size_t len = 0;
    CHECK_EQ(linkproto_encode(&h, pl, buf, sizeof(buf), &len), LINKPROTO_OK);

    static const uint8_t expect_hdr[28] = {
        'H',  'L',  'R',  'L',  1,    3,    0,    0,    0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x03,
    };
    CHECK(memcmp(buf, expect_hdr, sizeof(expect_hdr)) == 0);
    CHECK(memcmp(buf + 32, pl, 3) == 0);

    uint32_t crc = linkproto_crc32(0, buf, 28);
    crc = linkproto_crc32(crc, pl, 3);
    CHECK_EQ(linkproto_get_be32(buf + 28), crc);
}

static void test_encode_in_place_and_overlap(void)
{
    struct linkproto_hdr h;
    linkproto_hdr_init(&h, LINKPROTO_TYPE_ECHO_REQ, 7, 1, 2);
    h.payload_len = 100;

    linkproto_fill_pattern(buf + LINKPROTO_HDR_LEN, 100, 7);
    size_t len = 0;
    CHECK_EQ(linkproto_encode(&h, NULL, buf, sizeof(buf), &len), LINKPROTO_OK);
    CHECK_EQ(linkproto_decode(buf, len, NULL, NULL), LINKPROTO_OK);
    CHECK(linkproto_check_pattern(buf + LINKPROTO_HDR_LEN, 100, 7));

    /* Payload overlapping the destination (as an echo server re-encoding in place would). */
    linkproto_fill_pattern(buf + 10, 100, 7);
    CHECK_EQ(linkproto_encode(&h, buf + 10, buf, sizeof(buf), &len), LINKPROTO_OK);
    CHECK_EQ(linkproto_decode(buf, len, NULL, NULL), LINKPROTO_OK);
    CHECK(linkproto_check_pattern(buf + LINKPROTO_HDR_LEN, 100, 7));
}

static void test_encode_errors(void)
{
    struct linkproto_hdr h;
    size_t len = 0;

    linkproto_hdr_init(&h, LINKPROTO_TYPE_ECHO_REQ, 1, 2, 3);
    CHECK_EQ(linkproto_encode(NULL, NULL, buf, sizeof(buf), &len), LINKPROTO_ERR_ARG);
    CHECK_EQ(linkproto_encode(&h, NULL, NULL, sizeof(buf), &len), LINKPROTO_ERR_ARG);

    h.version = 2;
    CHECK_EQ(linkproto_encode(&h, NULL, buf, sizeof(buf), &len), LINKPROTO_ERR_VERSION);
    h.version = LINKPROTO_VERSION;

    h.type = 0;
    CHECK_EQ(linkproto_encode(&h, NULL, buf, sizeof(buf), &len), LINKPROTO_ERR_TYPE);
    h.type = 5;
    CHECK_EQ(linkproto_encode(&h, NULL, buf, sizeof(buf), &len), LINKPROTO_ERR_TYPE);
    h.type = LINKPROTO_TYPE_ECHO_REQ;

    h.flags = 0x80;
    CHECK_EQ(linkproto_encode(&h, NULL, buf, sizeof(buf), &len), LINKPROTO_ERR_FLAGS);
    h.flags = 0;

    h.status = 1;
    CHECK_EQ(linkproto_encode(&h, NULL, buf, sizeof(buf), &len), LINKPROTO_ERR_RESERVED);
    h.status = 0;

    h.payload_len = LINKPROTO_MAX_PAYLOAD + 1;
    CHECK_EQ(linkproto_encode(&h, NULL, buf, sizeof(buf), &len), LINKPROTO_ERR_LEN);

    h.payload_len = 10;
    CHECK_EQ(linkproto_encode(&h, NULL, buf, LINKPROTO_HDR_LEN + 9, &len), LINKPROTO_ERR_NOSPACE);
    CHECK_EQ(linkproto_encode(&h, NULL, buf, LINKPROTO_HDR_LEN + 10, &len), LINKPROTO_OK);
}

static void test_decode_validation(void)
{
    size_t len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);
    struct linkproto_hdr d;

    CHECK_EQ(linkproto_decode(NULL, len, &d, NULL), LINKPROTO_ERR_ARG);
    CHECK_EQ(linkproto_decode(buf, 0, &d, NULL), LINKPROTO_ERR_SHORT);
    CHECK_EQ(linkproto_decode(buf, LINKPROTO_HDR_LEN - 1, &d, NULL), LINKPROTO_ERR_SHORT);
    CHECK_EQ(linkproto_decode(buf, len - 1, &d, NULL), LINKPROTO_ERR_TRUNC);
    CHECK_EQ(linkproto_decode(buf, len + 1, &d, NULL), LINKPROTO_ERR_TRUNC);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_OK);

    /* Each header field individually wrong, CRC recomputed so the field check is what fires. */
    buf[0] = 'X';
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_MAGIC);
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);

    buf[4] = 0;
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_VERSION);
    buf[4] = 2;
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_VERSION);
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);

    buf[5] = 0;
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_TYPE);
    buf[5] = 5;
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_TYPE);
    buf[5] = 0xFF;
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_TYPE);
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);

    buf[6] = 0x02;
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_FLAGS);
    buf[6] = LINKPROTO_FLAG_LAST;
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_OK);
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);

    buf[7] = 1;
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_RESERVED);
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);

    /* Non-zero status is invalid in requests, valid in responses. */
    linkproto_put_be16(buf + 24, 1);
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_RESERVED);
    len = make_packet(LINKPROTO_TYPE_MSG, 20, 99);
    linkproto_put_be16(buf + 24, 1);
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_RESERVED);
    len = make_packet(LINKPROTO_TYPE_MSG_ACK, 20, 99);
    linkproto_put_be16(buf + 24, 0xBEEF);
    fix_crc(len);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_OK);
    CHECK_EQ(d.status, 0xBEEF);

    /* payload_len above the maximum, even when the buffer is that long. */
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 0, 1);
    linkproto_put_be16(buf + 26, LINKPROTO_MAX_PAYLOAD + 1);
    CHECK_EQ(linkproto_decode(buf, LINKPROTO_HDR_LEN + LINKPROTO_MAX_PAYLOAD + 1, &d, NULL),
             LINKPROTO_ERR_LEN);
    linkproto_put_be16(buf + 26, 0xFFFF);
    CHECK_EQ(linkproto_decode(buf, sizeof(buf), &d, NULL), LINKPROTO_ERR_LEN);

    /* payload_len that disagrees with the datagram length. */
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);
    linkproto_put_be16(buf + 26, 19);
    fix_crc(len - 1);
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_TRUNC);

    /* CRC field and payload corruption. */
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);
    buf[28] ^= 0x01;
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_CRC);
    len = make_packet(LINKPROTO_TYPE_ECHO_REQ, 20, 99);
    buf[LINKPROTO_HDR_LEN + 5] ^= 0x80;
    CHECK_EQ(linkproto_decode(buf, len, &d, NULL), LINKPROTO_ERR_CRC);
}

static void test_every_bit_flip_rejected(void)
{
    size_t len = make_packet(LINKPROTO_TYPE_MSG, 48, 12345);
    unsigned rejected = 0;
    unsigned total = 0;

    for (size_t i = 0; i < len; i++)
    {
        for (int bit = 0; bit < 8; bit++)
        {
            buf[i] ^= (uint8_t)(1U << bit);
            total++;
            if (linkproto_decode(buf, len, NULL, NULL) != LINKPROTO_OK)
            {
                rejected++;
            }
            buf[i] ^= (uint8_t)(1U << bit);
        }
    }
    CHECK_EQ(rejected, total);
    CHECK_EQ(linkproto_decode(buf, len, NULL, NULL), LINKPROTO_OK);
}

static void test_check_header_partial(void)
{
    size_t len = make_packet(LINKPROTO_TYPE_MSG, 200, 5);
    struct linkproto_hdr h;

    /* Only the header is available; the payload may still be in flight. */
    CHECK_EQ(linkproto_check_header(buf, LINKPROTO_HDR_LEN, len, &h), LINKPROTO_OK);
    CHECK_EQ(h.payload_len, 200);
    CHECK_EQ(h.seq, 5);
    CHECK_EQ(linkproto_check_header(buf, LINKPROTO_HDR_LEN, len + 1, &h), LINKPROTO_ERR_TRUNC);
    CHECK_EQ(linkproto_check_header(buf, LINKPROTO_HDR_LEN - 1, len, &h), LINKPROTO_ERR_SHORT);
    CHECK_EQ(linkproto_check_header(buf, LINKPROTO_HDR_LEN, 10, &h), LINKPROTO_ERR_SHORT);
    CHECK_EQ(linkproto_check_header(NULL, LINKPROTO_HDR_LEN, len, &h), LINKPROTO_ERR_ARG);
}

static void test_pattern(void)
{
    uint8_t a[64];
    uint8_t b[64];

    linkproto_fill_pattern(a, sizeof(a), 1);
    linkproto_fill_pattern(b, sizeof(b), 2);
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    CHECK(linkproto_check_pattern(a, sizeof(a), 1));
    CHECK(!linkproto_check_pattern(a, sizeof(a), 2));
    a[63] ^= 1;
    CHECK(!linkproto_check_pattern(a, sizeof(a), 1));
    CHECK(linkproto_check_pattern(a, 0, 1));
}

static void test_result_strings(void)
{
    for (int r = LINKPROTO_OK; r <= LINKPROTO_ERR_NOSPACE; r++)
    {
        const char *s = linkproto_result_str((enum linkproto_result)r);
        CHECK(s != NULL && strcmp(s, "unknown") != 0);
    }
    CHECK(strcmp(linkproto_result_str(LINKPROTO_ERR_CRC), "crc") == 0);
}

static void test_byte_helpers(void)
{
    uint8_t b[8];
    linkproto_put_be64(b, 0x0123456789ABCDEFULL);
    CHECK_EQ(b[0], 0x01);
    CHECK_EQ(b[7], 0xEF);
    CHECK_EQ(linkproto_get_be64(b), 0x0123456789ABCDEFULL);
    linkproto_put_be16(b, 0xA55A);
    CHECK_EQ(linkproto_get_be16(b), 0xA55A);
    linkproto_put_be32(b, 0x80000001UL);
    CHECK_EQ(linkproto_get_be32(b), 0x80000001UL);
}

static const struct test_case cases[] = {
    { "linkproto/crc32_vectors", test_crc32_vectors },
    { "linkproto/roundtrip_sizes", test_roundtrip_sizes },
    { "linkproto/wire_layout", test_wire_layout },
    { "linkproto/encode_in_place_and_overlap", test_encode_in_place_and_overlap },
    { "linkproto/encode_errors", test_encode_errors },
    { "linkproto/decode_validation", test_decode_validation },
    { "linkproto/every_bit_flip_rejected", test_every_bit_flip_rejected },
    { "linkproto/check_header_partial", test_check_header_partial },
    { "linkproto/pattern", test_pattern },
    { "linkproto/result_strings", test_result_strings },
    { "linkproto/byte_helpers", test_byte_helpers },
};

void test_linkproto_register(const struct test_case **out, size_t *n)
{
    *out = cases;
    *n = sizeof(cases) / sizeof(cases[0]);
}
