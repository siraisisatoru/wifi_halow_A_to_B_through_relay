/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Minimal test harness: CHECK* record a failure and continue the test.
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern unsigned test_checks;
extern unsigned test_failures;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        test_checks++;                                                       \
        if (!(cond))                                                         \
        {                                                                    \
            test_failures++;                                                 \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do                                                                       \
    {                                                                        \
        uint64_t va_ = (uint64_t)(a);                                        \
        uint64_t vb_ = (uint64_t)(b);                                        \
        test_checks++;                                                       \
        if (va_ != vb_)                                                      \
        {                                                                    \
            test_failures++;                                                 \
            printf("  FAIL %s:%d: %s == %s (%" PRIu64 " != %" PRIu64 ")\n", \
                   __FILE__,                                                 \
                   __LINE__,                                                 \
                   #a,                                                       \
                   #b,                                                       \
                   va_,                                                      \
                   vb_);                                                     \
        }                                                                    \
    } while (0)

typedef void (*test_fn_t)(void);

struct test_case
{
    const char *name;
    test_fn_t fn;
};

void test_linkproto_register(const struct test_case **cases, size_t *n);
void test_linkstats_register(const struct test_case **cases, size_t *n);
void test_msgstream_register(const struct test_case **cases, size_t *n);

/** Deterministic PRNG for reproducible random splits/corruptions. */
uint32_t test_rand(void);
void test_srand(uint32_t seed);

#endif /* TEST_HARNESS_H */
