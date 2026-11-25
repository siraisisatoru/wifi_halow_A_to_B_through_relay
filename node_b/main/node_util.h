/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Small helpers shared by the node A and node B applications (each project carries an identical
 * copy): MAC parsing/formatting, static IPv4 setup on the HaLow netif, static ARP entries, and
 * the periodic health line.
 *
 * Lines printed here use the common STATS/EVENT conventions (see common/README.md), except that
 * t_ms is milliseconds since boot rather than since a service started.
 */

#ifndef NODE_UTIL_H
#define NODE_UTIL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#define NODE_MAC_LEN     6
#define NODE_MAC_STR_LEN 18

/** Parse "aa:bb:cc:dd:ee:ff" (':' or '-' separators). Returns false for "" or malformed input. */
bool node_parse_mac(const char *str, uint8_t mac[NODE_MAC_LEN]);

void node_format_mac(const uint8_t mac[NODE_MAC_LEN], char out[NODE_MAC_STR_LEN]);

bool node_mac_is_zero(const uint8_t mac[NODE_MAC_LEN]);

bool node_mac_equal(const uint8_t a[NODE_MAC_LEN], const uint8_t b[NODE_MAC_LEN]);

/** Milliseconds since boot (wraps after ~49 days; compare with node_time_reached()). */
uint32_t node_uptime_ms(void);

/** True once @p now_ms has reached @p deadline_ms, tolerant of 32-bit wrap. */
bool node_time_reached(uint32_t now_ms, uint32_t deadline_ms);

/**
 * Give @p netif a static IPv4 address: stops the DHCP client that esp-halow creates the netif
 * with (and a DHCP server, if the netif has one), then sets the address. esp_netif keeps it
 * across link down/up, and the next link up posts IP_EVENT_STA_GOT_IP with it.
 */
esp_err_t node_set_static_ip(esp_netif_t *netif,
                             const char *ip,
                             const char *netmask,
                             const char *gateway);

/**
 * Install a static ARP entry @p ip -> @p mac_str in lwIP and print an EVENT,arp line.
 * Does nothing when @p mac_str is empty. lwIP drops all ARP entries, static ones included, when
 * the netif goes down, so callers re-install after every link up. Needs a route to @p ip (the
 * netif up with its address), otherwise lwIP returns ERR_RTE.
 *
 * @param name      Peer label for the EVENT line (e.g. "node_a").
 * @param quiet     Only print on failure (periodic refresh).
 */
esp_err_t node_static_arp_set(const char *name, const char *ip, const char *mac_str, bool quiet);

/** Print EVENT,node,...,ev=health with uptime and heap figures, followed by @p extra. */
void node_log_health(const char *extra);

#endif /* NODE_UTIL_H */
