/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"

#include "lab_platform.h"
#include "node_util.h"

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

bool node_parse_mac(const char *str, uint8_t mac[NODE_MAC_LEN])
{
    uint8_t out[NODE_MAC_LEN];

    if (str == NULL)
    {
        return false;
    }

    for (int ii = 0; ii < NODE_MAC_LEN; ii++)
    {
        int hi = hex_nibble(str[0]);
        if (hi < 0)
        {
            return false;
        }
        int lo = hex_nibble(str[1]);
        if (lo < 0)
        {
            return false;
        }
        out[ii] = (uint8_t)((hi << 4) | lo);
        str += 2;

        if (ii < NODE_MAC_LEN - 1)
        {
            if (*str != ':' && *str != '-')
            {
                return false;
            }
            str++;
        }
    }
    if (*str != '\0')
    {
        return false;
    }

    memcpy(mac, out, NODE_MAC_LEN);
    return true;
}

void node_format_mac(const uint8_t mac[NODE_MAC_LEN], char out[NODE_MAC_STR_LEN])
{
    snprintf(out,
             NODE_MAC_STR_LEN,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

bool node_mac_is_zero(const uint8_t mac[NODE_MAC_LEN])
{
    for (int ii = 0; ii < NODE_MAC_LEN; ii++)
    {
        if (mac[ii] != 0)
        {
            return false;
        }
    }
    return true;
}

bool node_mac_equal(const uint8_t a[NODE_MAC_LEN], const uint8_t b[NODE_MAC_LEN])
{
    return memcmp(a, b, NODE_MAC_LEN) == 0;
}

uint32_t node_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool node_time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

esp_err_t node_set_static_ip(esp_netif_t *netif,
                             const char *ip,
                             const char *netmask,
                             const char *gateway)
{
    esp_netif_ip_info_t info = { 0 };

    if (esp_netif_str_to_ip4(ip, &info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(netmask, &info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(gateway, &info.gw) != ESP_OK)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        return err;
    }

#if CONFIG_LWIP_DHCPS
    /* The esp-halow netif is a DHCP client only; this guards against that changing. A DHCP
     * server would also add and remove temporary static ARP entries for its clients. */
    if (esp_netif_get_flags(netif) & ESP_NETIF_DHCP_SERVER)
    {
        err = esp_netif_dhcps_stop(netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
        {
            return err;
        }
    }
#endif

    return esp_netif_set_ip_info(netif, &info);
}

#if ETHARP_SUPPORT_STATIC_ENTRIES
struct static_arp_req
{
    ip4_addr_t ip;
    struct eth_addr mac;
    err_t err;
};

/* Runs in the lwIP thread (etharp_* asserts the core lock). */
static esp_err_t static_arp_add_cb(void *ctx)
{
    struct static_arp_req *req = (struct static_arp_req *)ctx;

    req->err = etharp_add_static_entry(&req->ip, &req->mac);
    return (req->err == ERR_OK) ? ESP_OK : ESP_FAIL;
}
#endif

esp_err_t node_static_arp_set(const char *name, const char *ip, const char *mac_str, bool quiet)
{
    uint8_t mac[NODE_MAC_LEN];
    char mac_buf[NODE_MAC_STR_LEN];

    if (mac_str == NULL || mac_str[0] == '\0')
    {
        return ESP_OK;
    }
    if (!node_parse_mac(mac_str, mac))
    {
        lab_outf("EVENT,arp,t_ms=%" PRIu32 ",ev=static_fail,peer=%s,ip=%s,mac=%s,err=bad_mac",
                 node_uptime_ms(),
                 name,
                 ip,
                 mac_str);
        return ESP_ERR_INVALID_ARG;
    }
    node_format_mac(mac, mac_buf);

#if ETHARP_SUPPORT_STATIC_ENTRIES
    struct static_arp_req req = { 0 };
    if (!ip4addr_aton(ip, &req.ip))
    {
        lab_outf("EVENT,arp,t_ms=%" PRIu32 ",ev=static_fail,peer=%s,ip=%s,mac=%s,err=bad_ip",
                 node_uptime_ms(),
                 name,
                 ip,
                 mac_buf);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(req.mac.addr, mac, NODE_MAC_LEN);

    esp_err_t err = esp_netif_tcpip_exec(static_arp_add_cb, &req);
    if (err != ESP_OK || !quiet)
    {
        lab_outf("EVENT,arp,t_ms=%" PRIu32 ",ev=%s,peer=%s,ip=%s,mac=%s,err=%d",
                 node_uptime_ms(),
                 (err == ESP_OK) ? "static" : "static_fail",
                 name,
                 ip,
                 mac_buf,
                 (int)req.err);
    }
    return err;
#else
    (void)quiet;
    lab_outf("EVENT,arp,t_ms=%" PRIu32 ",ev=static_fail,peer=%s,ip=%s,mac=%s,err=unsupported",
             node_uptime_ms(),
             name,
             ip,
             mac_buf);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void node_log_health(const char *extra)
{
    uint32_t now = node_uptime_ms();

    lab_outf("EVENT,node,t_ms=%" PRIu32 ",ev=health,uptime_s=%" PRIu32 ",heap_free=%" PRIu32
             ",heap_min=%" PRIu32 "%s",
             now,
             now / 1000,
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(),
             (extra != NULL) ? extra : "");
}
