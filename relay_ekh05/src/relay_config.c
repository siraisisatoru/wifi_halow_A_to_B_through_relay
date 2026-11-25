/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmconfig.h"
#include "mmosal.h"
#include "mmregdb.h"
#include "mmutils.h"

#include "halow_lab_config.h"
#include "lab_platform.h"
#include "relay_config.h"
#include "relay_events.h"

/** Attempts restricted to the selected channels before the STA falls back to a full scan. */
#define RELAY_DEFAULT_SCAN_ATTEMPTS 2

/** Interval between ev=health lines. */
#define RELAY_DEFAULT_HEALTH_INTERVAL_MS 10000

#define RELAY_DEFAULT_HOSTNAME "halow-relay"

/* The heap and packet pools are sized for MAX_STAS at build time (Makefile). */
#if MAX_STAS < 1 || MAX_STAS > MMWLAN_AP_MAX_STAS_LIMIT
#error "MAX_STAS must be between 1 and MMWLAN_AP_MAX_STAS_LIMIT"
#endif

bool relay_parse_mac(const char *str, uint8_t *mac)
{
    for (unsigned ii = 0; ii < MMWLAN_MAC_ADDR_LEN; ii++)
    {
        char *end;
        unsigned long v = strtoul(str, &end, 16);

        if (end == str || end - str > 2 || v > 0xff)
        {
            return false;
        }
        mac[ii] = (uint8_t)v;
        if (ii == MMWLAN_MAC_ADDR_LEN - 1)
        {
            return *end == '\0';
        }
        if (*end != ':' && *end != '-')
        {
            return false;
        }
        str = end + 1;
    }
    return false;
}

const char *relay_security_str(enum mmwlan_security_type type)
{
    switch (type)
    {
        case MMWLAN_SAE:
            return "sae";

        case MMWLAN_OWE:
            return "owe";

        case MMWLAN_OPEN:
            return "open";
    }
    return "unknown";
}

/*
 * Read a string key into @p buf; true only if the key exists, fits and is non-empty. @p buf is
 * left untouched otherwise, so an empty value cannot wipe a default.
 */
static bool read_str(const char *key, char *buf, size_t size)
{
    char tmp[MMWLAN_PASSPHRASE_MAXLEN + 1];

    if (size > sizeof(tmp) || mmconfig_read_string(key, tmp, (int)size) <= 0 || tmp[0] == '\0')
    {
        return false;
    }
    memcpy(buf, tmp, strlen(tmp) + 1);
    return true;
}

/* Strict dotted-quad IPv4 ("a.b.c.d", each 0-255); a subset of what mmipal/lwIP accepts. */
static bool is_ipv4_dotted_quad(const char *str)
{
    for (unsigned ii = 0; ii < 4; ii++)
    {
        unsigned v = 0;
        unsigned digits = 0;

        while (*str >= '0' && *str <= '9')
        {
            v = v * 10 + (unsigned)(*str - '0');
            if (++digits > 3 || v > 255)
            {
                return false;
            }
            str++;
        }
        if (digits == 0 || *str != (ii == 3 ? '\0' : '.'))
        {
            return false;
        }
        str++;
    }
    return true;
}

/* Read an IPv4 address key; a malformed value is logged and leaves @p buf unchanged. */
static void read_ipv4(const char *key, char *buf, size_t size)
{
    mmipal_ip_addr_t tmp;

    if (size > sizeof(tmp) || !read_str(key, tmp, size))
    {
        return;
    }
    if (!is_ipv4_dotted_quad(tmp))
    {
        lab_log(LAB_LOG_ERR, "config: %s=%s invalid, ignored", key, tmp);
        return;
    }
    memcpy(buf, tmp, strlen(tmp) + 1);
}

static void read_u32_max(const char *key, uint32_t max, uint32_t *value)
{
    uint32_t v;

    if (mmconfig_read_uint32(key, &v) != MMCONFIG_OK)
    {
        return;
    }
    if (v > max)
    {
        lab_log(LAB_LOG_ERR, "config: %s=%lu out of range (max %lu), ignored", key,
                (unsigned long)v, (unsigned long)max);
        return;
    }
    *value = v;
}

/* mmconfig leaves @p mac untouched when the key is missing or malformed. */
static void read_mac(const char *key, uint8_t *mac)
{
    (void)mmconfig_read_mac_addr(key, mac);
}

/* Parse "37" or "33,37,41". Returns the number of channels, 0 if malformed. */
static uint8_t parse_channel_list(const char *str, uint8_t *channels, uint8_t max)
{
    uint8_t count = 0;

    while (count < max)
    {
        char *end;
        unsigned long v = strtoul(str, &end, 10);

        if (end == str || v == 0 || v > UINT8_MAX)
        {
            return 0;
        }
        channels[count++] = (uint8_t)v;
        if (*end == '\0')
        {
            return count;
        }
        if (*end != ',')
        {
            return 0;
        }
        str = end + 1;
    }
    return 0;
}

static void load_defaults(struct relay_cfg *cfg)
{
    static const struct mmwlan_sta_args sta_init = MMWLAN_STA_ARGS_INIT;
    static const struct mmwlan_relay_ap_args ap_init = MMWLAN_RELAY_AP_ARGS_INIT;
    static const struct mmipal_init_args ip_init = MMIPAL_INIT_ARGS_DEFAULT;
    static const uint8_t relay_ap_bssid[MMWLAN_MAC_ADDR_LEN] = LAB_RELAY_AP_BSSID_INIT;
    static const uint8_t root_ap_bssid[MMWLAN_MAC_ADDR_LEN] = LAB_ROOT_AP_BSSID_INIT;

    memset(cfg, 0, sizeof(*cfg));

    mmosal_safer_strcpy(cfg->country, LAB_COUNTRY, sizeof(cfg->country));

    cfg->sta_args = sta_init;
    mmosal_safer_strcpy((char *)cfg->sta_args.ssid, LAB_SSID, sizeof(cfg->sta_args.ssid));
    cfg->sta_args.security_type = LAB_SECURITY_SAE ? MMWLAN_SAE : MMWLAN_OPEN;
    mmosal_safer_strcpy(cfg->sta_args.passphrase, LAB_PASSPHRASE,
                        sizeof(cfg->sta_args.passphrase));
    cfg->sta_args.pmf_mode = LAB_PMF_REQUIRED ? MMWLAN_PMF_REQUIRED : MMWLAN_PMF_DISABLED;
    cfg->sta_args.scan_interval_base_s = LAB_STA_SCAN_INTERVAL_BASE_S;
    cfg->sta_args.scan_interval_limit_s = LAB_STA_SCAN_INTERVAL_LIMIT_S;

    /* The string forms are the canonical (overridable) ones; the _INIT arrays are fallbacks. */
    if (!relay_parse_mac(LAB_ROOT_AP_BSSID_STR, cfg->sta_args.bssid))
    {
        memcpy(cfg->sta_args.bssid, root_ap_bssid, sizeof(cfg->sta_args.bssid));
    }

    cfg->ap_args = ap_init;
    if (!relay_parse_mac(LAB_RELAY_AP_BSSID_STR, cfg->ap_args.bssid))
    {
        memcpy(cfg->ap_args.bssid, relay_ap_bssid, sizeof(cfg->ap_args.bssid));
    }
    cfg->ap_args.beacon_interval_tus = 100;
    cfg->ap_args.dtim_period = 1;
    cfg->ap_args.max_stas = MAX_STAS;

    cfg->recovery_timeout_ms = MMWLAN_RELAY_DEFAULT_RECOVERY_TIMEOUT_MS;
    cfg->forwarding_table_size = MMWLAN_RELAY_DEFAULT_FORWARDING_TABLE_SIZE;

    cfg->scan_channels[0] = LAB_S1G_CHAN_NUM;
    cfg->num_scan_channels = 1;
    cfg->scan_attempts = RELAY_DEFAULT_SCAN_ATTEMPTS;

    cfg->ip_args = ip_init;
    cfg->ip_args.mode = MMIPAL_STATIC;
    cfg->ip_args.ip6_mode = MMIPAL_IP6_DISABLED;
    mmosal_safer_strcpy(cfg->ip_args.ip_addr, LAB_IP_RELAY, sizeof(cfg->ip_args.ip_addr));
    mmosal_safer_strcpy(cfg->ip_args.netmask, LAB_NETMASK, sizeof(cfg->ip_args.netmask));
    mmosal_safer_strcpy(cfg->ip_args.gateway_addr, LAB_GATEWAY,
                        sizeof(cfg->ip_args.gateway_addr));
    mmosal_safer_strcpy(cfg->ip_args.hostname, RELAY_DEFAULT_HOSTNAME,
                        sizeof(cfg->ip_args.hostname));

    cfg->udp_echo_port = LAB_UDP_ECHO_PORT;
    cfg->report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg->health_interval_ms = RELAY_DEFAULT_HEALTH_INTERVAL_MS;
    cfg->led_enabled = true;
}

static void load_wlan_overrides(struct relay_cfg *cfg)
{
    char strval[32];
    uint32_t u32;

    char country[sizeof(cfg->country)];
    if (read_str("wlan.country_code", country, sizeof(country)))
    {
        if (mmwlan_lookup_regulatory_domain(get_regulatory_db(), country) == NULL)
        {
            lab_log(LAB_LOG_ERR, "config: wlan.country_code=%s unknown to the regdb, ignored",
                    country);
        }
        else
        {
            memcpy(cfg->country, country, sizeof(country));
        }
    }

    (void)read_str("wlan.ssid", (char *)cfg->sta_args.ssid, sizeof(cfg->sta_args.ssid));
    (void)read_str("wlan.password", cfg->sta_args.passphrase, sizeof(cfg->sta_args.passphrase));

    if (read_str("wlan.security", strval, sizeof(strval)))
    {
        if (strcmp(strval, "sae") == 0)
        {
            cfg->sta_args.security_type = MMWLAN_SAE;
        }
        else if (strcmp(strval, "owe") == 0)
        {
            cfg->sta_args.security_type = MMWLAN_OWE;
        }
        else if (strcmp(strval, "open") == 0)
        {
            cfg->sta_args.security_type = MMWLAN_OPEN;
        }
        else
        {
            lab_log(LAB_LOG_ERR, "config: wlan.security=%s invalid, ignored", strval);
        }
    }

    if (read_str("wlan.pmf_mode", strval, sizeof(strval)))
    {
        if (strcmp(strval, "required") == 0)
        {
            cfg->sta_args.pmf_mode = MMWLAN_PMF_REQUIRED;
        }
        else if (strcmp(strval, "disabled") == 0)
        {
            cfg->sta_args.pmf_mode = MMWLAN_PMF_DISABLED;
        }
        else
        {
            lab_log(LAB_LOG_ERR, "config: wlan.pmf_mode=%s invalid, ignored", strval);
        }
    }

    /* Upstream (root) BSSID pin; all-zero means any BSS with the right SSID. */
    read_mac("wlan.bssid", cfg->sta_args.bssid);

    read_mac("wlan.ap_bssid", cfg->ap_args.bssid);

    u32 = cfg->ap_args.beacon_interval_tus;
    read_u32_max("wlan.ap_beacon_interval_tus", UINT16_MAX, &u32);
    cfg->ap_args.beacon_interval_tus = (uint16_t)u32;

    u32 = cfg->ap_args.dtim_period;
    read_u32_max("wlan.ap_dtim_period", UINT16_MAX, &u32);
    cfg->ap_args.dtim_period = (uint16_t)u32;

    /* Larger values need a rebuild with MAX_STAS=N, which sizes the heap and packet pools. */
    u32 = cfg->ap_args.max_stas;
    read_u32_max("wlan.max_stas", MAX_STAS, &u32);
    if (u32 == 0)
    {
        lab_log(LAB_LOG_ERR, "config: wlan.max_stas=0 invalid, ignored");
        u32 = cfg->ap_args.max_stas;
    }
    cfg->ap_args.max_stas = (uint8_t)u32;

    u32 = cfg->depth_override;
    read_u32_max("wlan.relay_depth_override", MMWLAN_RELAY_MAX_DEPTH, &u32);
    cfg->depth_override = (uint8_t)u32;

    read_u32_max("lab.relay_recovery_timeout_ms", UINT32_MAX, &cfg->recovery_timeout_ms);

    u32 = cfg->forwarding_table_size;
    read_u32_max("lab.relay_forwarding_table_size", 1024, &u32);
    cfg->forwarding_table_size = u32;

    u32 = cfg->sta_args.scan_interval_base_s;
    read_u32_max("wlan.sta_scan_interval_base_s", UINT16_MAX, &u32);
    cfg->sta_args.scan_interval_base_s = (uint16_t)u32;

    u32 = cfg->sta_args.scan_interval_limit_s;
    read_u32_max("wlan.sta_scan_interval_limit_s", UINT16_MAX, &u32);
    cfg->sta_args.scan_interval_limit_s = (uint16_t)u32;

    if (read_str("wlan.scan_selected_channels", strval, sizeof(strval)))
    {
        if (strcmp(strval, "none") == 0)
        {
            cfg->num_scan_channels = 0;
        }
        else
        {
            uint8_t n =
                parse_channel_list(strval, cfg->scan_channels, RELAY_CFG_MAX_SCAN_CHANNELS);
            if (n == 0)
            {
                lab_log(LAB_LOG_ERR, "config: wlan.scan_selected_channels=%s invalid, ignored",
                        strval);
            }
            else
            {
                cfg->num_scan_channels = n;
            }
        }
    }

    u32 = cfg->scan_attempts;
    read_u32_max("wlan.selective_scan_attempts", UINT8_MAX, &u32);
    cfg->scan_attempts = (uint8_t)u32;
}

static void load_ip_overrides(struct relay_cfg *cfg)
{
    bool dhcp;
    char key[24];

    read_ipv4("ip.ip_addr", cfg->ip_args.ip_addr, sizeof(cfg->ip_args.ip_addr));
    read_ipv4("ip.netmask", cfg->ip_args.netmask, sizeof(cfg->ip_args.netmask));
    read_ipv4("ip.gateway", cfg->ip_args.gateway_addr, sizeof(cfg->ip_args.gateway_addr));
    (void)read_str("ip.hostname", cfg->ip_args.hostname, sizeof(cfg->ip_args.hostname));

    /* DHCP is possible but not recommended in this lab (see README, issue #10). */
    if (mmconfig_read_bool("ip.dhcp_enabled", &dhcp) == MMCONFIG_OK)
    {
        cfg->ip_args.mode = dhcp ? MMIPAL_DHCP : MMIPAL_STATIC;
    }

    for (unsigned ii = 0; ii < RELAY_CFG_MAX_STATIC_ARP; ii++)
    {
        struct relay_static_arp *arp = &cfg->static_arp[cfg->num_static_arp];

        snprintf(key, sizeof(key), "lab.arp%u.ip", ii);
        if (!read_str(key, arp->ip, sizeof(arp->ip)))
        {
            continue;
        }
        snprintf(key, sizeof(key), "lab.arp%u.mac", ii);
        if (mmconfig_read_mac_addr(key, arp->mac) != MMCONFIG_OK)
        {
            lab_log(LAB_LOG_ERR, "config: lab.arp%u.ip set without a valid lab.arp%u.mac", ii,
                    ii);
            continue;
        }
        cfg->num_static_arp++;
    }
}

static void load_lab_overrides(struct relay_cfg *cfg)
{
    uint32_t u32;
    bool b;

    u32 = cfg->udp_echo_port;
    read_u32_max("lab.udp_echo_port", UINT16_MAX, &u32);
    cfg->udp_echo_port = (uint16_t)u32;

    read_u32_max("lab.report_interval_ms", UINT32_MAX, &cfg->report_interval_ms);
    read_u32_max("lab.health_interval_ms", UINT32_MAX, &cfg->health_interval_ms);
    if (cfg->health_interval_ms < 1000)
    {
        cfg->health_interval_ms = 1000;
    }

    if (mmconfig_read_bool("lab.led_enabled", &b) == MMCONFIG_OK)
    {
        cfg->led_enabled = b;
    }
}

void relay_cfg_load(struct relay_cfg *cfg)
{
    load_defaults(cfg);
    load_wlan_overrides(cfg);
    load_ip_overrides(cfg);
    load_lab_overrides(cfg);

    cfg->sta_args.ssid_len = (uint16_t)strlen((const char *)cfg->sta_args.ssid);
    /* Always explicit: with passphrase_len == 0 mm-iot-sdk 2.14.3 uses strlen(ssid) (#11). */
    cfg->sta_args.passphrase_len = (uint16_t)strlen(cfg->sta_args.passphrase);
}

void relay_cfg_print(const struct relay_cfg *cfg)
{
    char scan[48] = "none";
    size_t pos = 0;

    for (unsigned ii = 0; ii < cfg->num_scan_channels && pos < sizeof(scan); ii++)
    {
        int n = snprintf(&scan[pos], sizeof(scan) - pos, "%s%u", ii ? "/" : "",
                         cfg->scan_channels[ii]);
        if (n < 0)
        {
            break;
        }
        pos += (size_t)n;
    }

    relay_event("config",
                "ssid=%s,security=%s,pmf=%s,passphrase_len=%u,country=%s,ap_bssid=" MM_MAC_ADDR_FMT
                ",root_bssid=" MM_MAC_ADDR_FMT ",beacon_tu=%u,dtim=%u,max_stas=%u,"
                "recovery_ms=%lu,fwd_table=%u,depth_override=%u,scan_chans=%s,scan_attempts=%u,"
                "scan_interval_s=%u/%u",
                (const char *)cfg->sta_args.ssid, relay_security_str(cfg->sta_args.security_type),
                cfg->sta_args.pmf_mode == MMWLAN_PMF_REQUIRED ? "required" : "disabled",
                cfg->sta_args.passphrase_len, cfg->country, MM_MAC_ADDR_VAL(cfg->ap_args.bssid),
                MM_MAC_ADDR_VAL(cfg->sta_args.bssid), cfg->ap_args.beacon_interval_tus,
                cfg->ap_args.dtim_period, cfg->ap_args.max_stas,
                (unsigned long)cfg->recovery_timeout_ms, cfg->forwarding_table_size,
                cfg->depth_override, scan, cfg->scan_attempts, cfg->sta_args.scan_interval_base_s,
                cfg->sta_args.scan_interval_limit_s);

    relay_event("ip_config",
                "mode=%s,ip=%s,netmask=%s,gateway=%s,hostname=%s,static_arp=%u,udp_echo_port=%u",
                cfg->ip_args.mode == MMIPAL_DHCP ? "dhcp" : "static", cfg->ip_args.ip_addr,
                cfg->ip_args.netmask, cfg->ip_args.gateway_addr, cfg->ip_args.hostname,
                cfg->num_static_arp, cfg->udp_echo_port);

    if (strcmp(cfg->sta_args.passphrase, "change-me-halow-au") == 0)
    {
        lab_log(LAB_LOG_WRN, "using the placeholder passphrase; set wlan.password for real use");
    }
}
