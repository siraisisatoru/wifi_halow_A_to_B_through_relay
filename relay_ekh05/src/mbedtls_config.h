/*
 * Copyright 2026 siraisisatoru
 * Copyright The Mbed TLS Contributors
 * Copyright 2023-2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * mbedTLS configuration for the relay firmware (selected by the SDK through
 * MBEDTLS_CONFIG_FILE="<mbedtls_config.h>"). Same content as the SDK relay_mode example: the
 * relay needs only what morselib and the WPA supplicant require (SAE, PMF, CCMP), which
 * mm_mbedtls_config.h enables.
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#include "mmosal.h"
#include "mmhal_app.h"

/* Generate errors if deprecated functions are used. */
#define MBEDTLS_DEPRECATED_REMOVED

/* Place AES tables in ROM. */
#define MBEDTLS_AES_ROM_TABLES

/* System support */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_MACRO(x) mmhal_get_time()
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO snprintf
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_CALLOC_MACRO mmosal_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO   mmosal_free

/* Ensure that mbedTLS features that are required for morselib are enabled. */
#include "mm_mbedtls_config.h"

#endif
