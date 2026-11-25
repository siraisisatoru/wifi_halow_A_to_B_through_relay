/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Small string helpers private to the common library.
 */

#ifndef LAB_UTIL_H
#define LAB_UTIL_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define LAB_PRINTF_LIKE(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define LAB_PRINTF_LIKE(fmt_idx, arg_idx)
#endif

/** Copy with truncation; always NUL-terminates when @p size > 0. */
void lab_strlcpy(char *dst, const char *src, size_t size);

/**
 * Decimal formatting of a 64-bit value without relying on printf "%llu" (newlib-nano on the
 * EKH05 does not support 64-bit conversions).
 */
void lab_u64_to_str(uint64_t v, char *buf, size_t size);

/**
 * snprintf at @p *pos. On overflow @p buf stays NUL-terminated and @p *pos is set to @p len so
 * later appends are no-ops and the caller can detect truncation (pos >= len).
 */
void lab_appendf(char *buf, size_t len, size_t *pos, const char *fmt, ...) LAB_PRINTF_LIKE(4, 5);

#endif /* LAB_UTIL_H */
