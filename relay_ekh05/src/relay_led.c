/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mmhal_app.h"
#include "mmosal.h"
#include "mmutils.h"
#include "mmwlan.h"

#include "relay_led.h"

#define LED_SEARCH_BLINK_MS 1000
#define LED_PULSE_MS 150
#define LED_PAUSE_MS 2000
#define LED_POLL_MS 200
#define LED_TASK_STACK_U32 256

static volatile bool led_enabled;
static volatile bool led_failed;
static volatile bool led_sta_connected;
static volatile uint8_t led_depth = MMWLAN_RELAY_DEPTH_UNKNOWN;
static volatile uint8_t led_ap_stas;

static void leds_off(void)
{
    mmhal_set_led(LED_RED, LED_OFF);
    mmhal_set_led(LED_GREEN, LED_OFF);
    mmhal_set_led(LED_BLUE, LED_OFF);
}

static void pulse(uint8_t led, unsigned count)
{
    for (unsigned ii = 0; ii < count && !led_failed; ii++)
    {
        mmhal_set_led(led, LED_ON);
        mmosal_task_sleep(LED_PULSE_MS);
        mmhal_set_led(led, LED_OFF);
        mmosal_task_sleep(LED_PULSE_MS);
    }
}

static void led_task_main(void *arg)
{
    MM_UNUSED(arg);

    while (true)
    {
        leds_off();

        if (led_failed)
        {
            mmhal_set_led(LED_RED, LED_ON);
            mmosal_task_sleep(LED_POLL_MS);
        }
        else if (!led_sta_connected)
        {
            mmhal_set_led(LED_BLUE, LED_ON);
            mmosal_task_sleep(LED_SEARCH_BLINK_MS);
            mmhal_set_led(LED_BLUE, LED_OFF);
            mmosal_task_sleep(LED_SEARCH_BLINK_MS);
        }
        else if (led_depth == MMWLAN_RELAY_DEPTH_UNKNOWN)
        {
            mmhal_set_led(LED_BLUE, LED_ON);
            mmosal_task_sleep(LED_POLL_MS);
        }
        else
        {
            pulse(LED_GREEN, led_depth);
            mmosal_task_sleep(LED_PULSE_MS * 2);
            pulse(LED_BLUE, led_ap_stas);
            mmosal_task_sleep(LED_PAUSE_MS);
        }
    }
}

void relay_led_init(bool enabled)
{
    led_enabled = enabled;
    if (!enabled)
    {
        return;
    }

    struct mmosal_task *task =
        mmosal_task_create(led_task_main, NULL, MMOSAL_TASK_PRI_LOW, LED_TASK_STACK_U32, "led");
    if (task == NULL)
    {
        led_enabled = false;
    }
}

void relay_led_update(bool sta_connected, uint8_t depth, uint8_t ap_stas)
{
    led_sta_connected = sta_connected;
    led_depth = depth;
    led_ap_stas = ap_stas;
}

void relay_led_set_failed(void)
{
    led_failed = true;
    if (!led_enabled)
    {
        return;
    }
    leds_off();
    mmhal_set_led(LED_RED, LED_ON);
}
