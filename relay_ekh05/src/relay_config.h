/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Relay settings: compiled-in defaults from halow_lab_config.h, optionally overridden by keys in
 * the mmconfig persistent config store (same key names as the SDK examples' config.hjson).
 */

#ifndef RELAY_CONFIG_H
#define RELAY_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "mmipal.h"
#include "mmwlan.h"

/** Maximum number of channels accepted in wlan.scan_selected_channels. */
#define RELAY_CFG_MAX_SCAN_CHANNELS 10

/** Maximum number of static ARP entries (lab.arpN.ip / lab.arpN.mac, N = 0..). */
#define RELAY_CFG_MAX_STATIC_ARP 4

struct relay_static_arp
{
    char ip[MMIPAL_IPADDR_STR_MAXLEN];
    uint8_t mac[MMWLAN_MAC_ADDR_LEN];
};

struct relay_cfg
{
    /** Country code used to select the channel list from mmregdb. */
    char country[MMWLAN_COUNTRY_CODE_LEN];

    /** Upstream (relay STA) settings; the relay AP copies SSID, security and PMF from these. */
    struct mmwlan_sta_args sta_args;

    /** Downstream (relay AP) settings: BSSID, beacon interval, DTIM, max STAs. */
    struct mmwlan_relay_ap_args ap_args;

    uint32_t recovery_timeout_ms;
    unsigned forwarding_table_size;
    /** wlan.relay_depth_override (0 = none). */
    uint8_t depth_override;

    /** Selective scan list (defaults to the lab channel) and attempts before a full scan. */
    uint8_t scan_channels[RELAY_CFG_MAX_SCAN_CHANNELS];
    uint8_t num_scan_channels;
    uint8_t scan_attempts;

    struct mmipal_init_args ip_args;

    struct relay_static_arp static_arp[RELAY_CFG_MAX_STATIC_ARP];
    uint8_t num_static_arp;

    /** UDP echo service port (0 disables the service). */
    uint16_t udp_echo_port;
    uint32_t report_interval_ms;
    uint32_t health_interval_ms;
    bool led_enabled;
};

/** Fill @p cfg with the lab defaults and apply config store overrides. */
void relay_cfg_load(struct relay_cfg *cfg);

/** Print the effective configuration as EVENT,relay,...,ev=config (passphrase not shown). */
void relay_cfg_print(const struct relay_cfg *cfg);

/** Parse "aa:bb:cc:dd:ee:ff" (also '-' separated). Returns false on malformed input. */
bool relay_parse_mac(const char *str, uint8_t *mac);

/** Return a static string naming @p type ("sae", "owe", "open"). */
const char *relay_security_str(enum mmwlan_security_type type);

#endif /* RELAY_CONFIG_H */
