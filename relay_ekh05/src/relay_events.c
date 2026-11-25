/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdio.h>

#include "mmosal.h"

#include "relay_events.h"

void relay_event(const char *name, const char *fmt, ...)
{
    char line[LAB_LINE_MAX];
    int n = snprintf(line, sizeof(line), "EVENT,relay,t_ms=%lu,ev=%s",
                     (unsigned long)mmosal_get_time_ms(), name);

    if (n > 0 && (size_t)n < sizeof(line) - 1 && fmt != NULL)
    {
        va_list ap;

        line[n++] = ',';
        va_start(ap, fmt);
        (void)vsnprintf(&line[n], sizeof(line) - (size_t)n, fmt, ap);
        va_end(ap);
    }
    lab_out_line(line);
}
