/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdio.h>

#include "lab_platform.h"

void lab_outf(const char *fmt, ...)
{
    char line[LAB_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    lab_out_line(line);
}

void lab_log(enum lab_log_level level, const char *fmt, ...)
{
    char msg[LAB_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    lab_platform_log_write(level, msg);
}
