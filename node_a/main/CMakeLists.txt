# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "app_main.c" "node_util.c"
    INCLUDE_DIRS "."
    REQUIRES halow common esp_netif esp_event esp_timer esp_system lwip nvs_flash
)
