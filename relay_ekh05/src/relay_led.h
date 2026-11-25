/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Relay status on the EKH05 RGB LED (same idea as the SDK relay_mode example):
 *
 * | Pattern                                   | Meaning                                       |
 * |-------------------------------------------|-----------------------------------------------|
 * | blue, slow blink (1 s on / 1 s off)       | relay STA searching for / connecting to root  |
 * | blue, solid                               | associated, relay depth not known yet         |
 * | green pulses (N = depth), then blue pulses (M = authorized downstream STAs), pause | running |
 * | red, solid                                | fatal error (configuration, boot or relay)    |
 */

#ifndef RELAY_LED_H
#define RELAY_LED_H

#include <stdbool.h>
#include <stdint.h>

/** Start the LED task. Does nothing when @p enabled is false (all setters become no-ops). */
void relay_led_init(bool enabled);

/** Update the displayed state. @p depth is MMWLAN_RELAY_DEPTH_UNKNOWN when detached. */
void relay_led_update(bool sta_connected, uint8_t depth, uint8_t ap_stas);

/** Show the fatal error pattern (sticky). */
void relay_led_set_failed(void);

#endif /* RELAY_LED_H */
