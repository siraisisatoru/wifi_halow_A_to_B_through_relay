/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include "mmhal_core.h"
#include "mmosal.h"

#include "lab_platform.h"

/* mmosal_get_time_ms() wraps every ~49.7 days; extend it to 64 bits. */
static uint32_t last_ms;
static uint64_t wrap_ms;

void lab_platform_init(void)
{
}

uint64_t lab_time_us(void)
{
    uint64_t ms;

    MMOSAL_TASK_ENTER_CRITICAL();
    uint32_t now = mmosal_get_time_ms();
    if (now < last_ms)
    {
        wrap_ms += 1ULL << 32;
    }
    last_ms = now;
    ms = wrap_ms + now;
    MMOSAL_TASK_EXIT_CRITICAL();

    return ms * 1000ULL;
}

void lab_sleep_ms(uint32_t ms)
{
    mmosal_task_sleep(ms);
}

void lab_out_line(const char *line)
{
    printf("%s\n", line);
}

void lab_platform_log_write(enum lab_log_level level, const char *msg)
{
    static const char tags[] = { 'E', 'W', 'I', 'D' };
    char tag = (unsigned)level < sizeof(tags) ? tags[level] : '?';

    printf("%c lab: %s\n", tag, msg);
}

uint32_t lab_random_u32(void)
{
    return mmhal_random_u32(0, UINT32_MAX);
}

void *lab_malloc(size_t size)
{
    return mmosal_malloc(size);
}

void lab_free(void *p)
{
    mmosal_free(p);
}

int lab_task_create(lab_task_fn_t fn,
                    void *arg,
                    const char *name,
                    uint32_t stack_bytes,
                    enum lab_task_prio prio)
{
    enum mmosal_task_priority mm_prio;

    switch (prio)
    {
        case LAB_TASK_PRIO_LOW:
            mm_prio = MMOSAL_TASK_PRI_LOW;
            break;

        case LAB_TASK_PRIO_HIGH:
            mm_prio = MMOSAL_TASK_PRI_HIGH;
            break;

        default:
            mm_prio = MMOSAL_TASK_PRI_NORM;
            break;
    }

    /* mmosal tasks may return; the shim deletes the task afterwards. */
    struct mmosal_task *task = mmosal_task_create(fn, arg, mm_prio, (stack_bytes + 3U) / 4U, name);
    return task != NULL ? 0 : -1;
}
