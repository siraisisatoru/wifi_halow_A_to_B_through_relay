/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lab_platform.h"

static const char *TAG = "lab";

void lab_platform_init(void)
{
}

uint64_t lab_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

void lab_sleep_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

void lab_out_line(const char *line)
{
    /* newlib holds the stdout lock for the whole call, so the line is not interleaved. */
    printf("%s\n", line);
}

void lab_platform_log_write(enum lab_log_level level, const char *msg)
{
    switch (level)
    {
        case LAB_LOG_ERR:
            ESP_LOGE(TAG, "%s", msg);
            break;

        case LAB_LOG_WRN:
            ESP_LOGW(TAG, "%s", msg);
            break;

        case LAB_LOG_INF:
            ESP_LOGI(TAG, "%s", msg);
            break;

        default:
            ESP_LOGD(TAG, "%s", msg);
            break;
    }
}

uint32_t lab_random_u32(void)
{
    return esp_random();
}

void *lab_malloc(size_t size)
{
    return malloc(size);
}

void lab_free(void *p)
{
    free(p);
}

struct task_start
{
    lab_task_fn_t fn;
    void *arg;
};

static void task_trampoline(void *p)
{
    struct task_start start = *(struct task_start *)p;
    free(p);
    start.fn(start.arg);
    vTaskDelete(NULL);
}

int lab_task_create(lab_task_fn_t fn,
                    void *arg,
                    const char *name,
                    uint32_t stack_bytes,
                    enum lab_task_prio prio)
{
    UBaseType_t freertos_prio;

    switch (prio)
    {
        case LAB_TASK_PRIO_LOW:
            freertos_prio = tskIDLE_PRIORITY + 2;
            break;

        case LAB_TASK_PRIO_HIGH:
            freertos_prio = tskIDLE_PRIORITY + 8;
            break;

        default:
            freertos_prio = tskIDLE_PRIORITY + 5;
            break;
    }

    struct task_start *start = (struct task_start *)malloc(sizeof(*start));
    if (start == NULL)
    {
        return -1;
    }
    start->fn = fn;
    start->arg = arg;

    /* ESP-IDF FreeRTOS takes the stack depth in bytes. */
    if (xTaskCreate(task_trampoline, name, stack_bytes, start, freertos_prio, NULL) != pdPASS)
    {
        free(start);
        return -1;
    }
    return 0;
}
