/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Minimal platform layer used by the traffic services.
 *
 * Implementations: port/posix (Linux/macOS host), port/esp_idf (FreeRTOS + esp_timer +
 * ESP_LOG), port/mmosal (mm-iot-sdk). Exactly one is linked into a build.
 */

#ifndef LAB_PLATFORM_H
#define LAB_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(__GNUC__)
#define LAB_PLATFORM_PRINTF_LIKE(fmt_idx, arg_idx) \
    __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define LAB_PLATFORM_PRINTF_LIKE(fmt_idx, arg_idx)
#endif

/** Longest line lab_outf()/lab_log() format (longer output is truncated). */
#define LAB_LINE_MAX 512

enum lab_log_level
{
    LAB_LOG_ERR,
    LAB_LOG_WRN,
    LAB_LOG_INF,
    LAB_LOG_DBG,
};

enum lab_task_prio
{
    LAB_TASK_PRIO_LOW,
    LAB_TASK_PRIO_NORMAL,
    LAB_TASK_PRIO_HIGH,
};

typedef void (*lab_task_fn_t)(void *arg);

/** One-time setup (POSIX: ignore SIGPIPE, line-buffer stdout). Safe to call repeatedly. */
void lab_platform_init(void);

/**
 * Monotonic time in microseconds. Resolution: 1 us on POSIX and ESP-IDF (esp_timer),
 * 1 ms on mmosal (mmosal_get_time_ms extended to 64 bits).
 */
uint64_t lab_time_us(void);

void lab_sleep_ms(uint32_t ms);

/**
 * Write one machine-readable line (STATS/EVENT) to the console followed by '\n', without a
 * log prefix, in a single write so lines from different tasks do not interleave.
 */
void lab_out_line(const char *line);

/** printf-style wrapper around lab_out_line(). */
void lab_outf(const char *fmt, ...) LAB_PLATFORM_PRINTF_LIKE(1, 2);

/** Human-readable diagnostic log (ESP_LOGx on ESP-IDF, stderr on POSIX, printf on mmosal). */
void lab_log(enum lab_log_level level, const char *fmt, ...) LAB_PLATFORM_PRINTF_LIKE(2, 3);

/** Platform random number (not cryptographic). */
uint32_t lab_random_u32(void);

void *lab_malloc(size_t size);

void lab_free(void *p);

/**
 * Start @p fn(@p arg) in a new task/thread. The task ends when @p fn returns.
 *
 * @param stack_bytes   Stack size in bytes (ignored on POSIX).
 *
 * @returns 0 on success, -1 on failure.
 */
int lab_task_create(lab_task_fn_t fn,
                    void *arg,
                    const char *name,
                    uint32_t stack_bytes,
                    enum lab_task_prio prio);

/** Port hook behind lab_log(): print an already formatted message. */
void lab_platform_log_write(enum lab_log_level level, const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* LAB_PLATFORM_H */
