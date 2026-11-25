/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Machine-parseable relay console lines:
 *
 *     EVENT,relay,t_ms=<ms since boot>,ev=<name>[,key=value...]
 *
 * Same shape as the common library's EVENT lines (see common/README.md), so one parser
 * (regex ^(STATS|EVENT), comma-separated key=value, ignore unknown keys) handles all nodes.
 */

#ifndef RELAY_EVENTS_H
#define RELAY_EVENTS_H

#include "lab_platform.h"

/**
 * Print one EVENT,relay line. @p fmt formats the key=value fields after "ev=<name>"; pass NULL
 * when there are none. Lines longer than LAB_LINE_MAX are truncated.
 */
void relay_event(const char *name, const char *fmt, ...) LAB_PLATFORM_PRINTF_LIKE(2, 3);

#endif /* RELAY_EVENTS_H */
