/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Helpers shared by the service loops (private).
 */

#ifndef SVC_COMMON_H
#define SVC_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#include "lab_platform.h"
#include "lab_services.h"

static inline bool svc_stop_requested(const volatile bool *stop)
{
    return stop != NULL && *stop;
}

/** Milliseconds since @p start_us. */
static inline uint32_t svc_ms_since(uint64_t start_us, uint64_t now_us)
{
    return now_us > start_us ? (uint32_t)((now_us - start_us) / 1000U) : 0;
}

/** Shrink @p *timeout_ms so a wait ends no later than @p deadline_us. */
static inline void svc_clamp_timeout(uint32_t *timeout_ms, uint64_t now_us, uint64_t deadline_us)
{
    uint32_t ms = 0;
    if (deadline_us > now_us)
    {
        uint64_t d = (deadline_us - now_us + 999U) / 1000U;
        ms = d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
    }
    if (ms < *timeout_ms)
    {
        *timeout_ms = ms;
    }
}

#endif /* SVC_COMMON_H */
