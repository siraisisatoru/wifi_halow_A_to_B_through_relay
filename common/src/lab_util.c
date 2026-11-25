/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lab_util.h"

void lab_strlcpy(char *dst, const char *src, size_t size)
{
    if (size == 0)
    {
        return;
    }
    size_t n = strlen(src);
    if (n >= size)
    {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void lab_u64_to_str(uint64_t v, char *buf, size_t size)
{
    char tmp[21];
    size_t n = 0;

    do
    {
        tmp[n++] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    } while (v != 0 && n < sizeof(tmp));

    if (size == 0)
    {
        return;
    }
    size_t i = 0;
    while (n > 0 && i + 1 < size)
    {
        buf[i++] = tmp[--n];
    }
    buf[i] = '\0';
}

void lab_appendf(char *buf, size_t len, size_t *pos, const char *fmt, ...)
{
    if (*pos >= len)
    {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, len - *pos, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= len - *pos)
    {
        *pos = len;
    }
    else
    {
        *pos += (size_t)n;
    }
}
