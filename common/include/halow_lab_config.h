/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Canonical defaults for the A -> relay -> B HaLow lab (network plan).
 *
 * Every value is guarded by @c #ifndef so a build can override it:
 *  - ESP-IDF: a Kconfig option named @c CONFIG_<NAME> (e.g. @c CONFIG_LAB_SSID) is picked up
 *    automatically because @c sdkconfig.h is included below.
 *  - Any build: define @c LAB_CONFIG_OVERRIDE_HEADER to a header name (e.g.
 *    @c -DLAB_CONFIG_OVERRIDE_HEADER='"my_lab.h"') that is included first, or pass @c -D flags.
 *
 * All three nodes MUST agree on SSID, security, country, channel list and channel.
 */

#ifndef HALOW_LAB_CONFIG_H
#define HALOW_LAB_CONFIG_H

#ifdef LAB_CONFIG_OVERRIDE_HEADER
#include LAB_CONFIG_OVERRIDE_HEADER
#endif

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

/* Adopt a CONFIG_<NAME> (Kconfig) value for NAME if one exists and NAME is not yet set. */
#if !defined(LAB_SSID) && defined(CONFIG_LAB_SSID)
#define LAB_SSID CONFIG_LAB_SSID
#endif
#if !defined(LAB_PASSPHRASE) && defined(CONFIG_LAB_PASSPHRASE)
#define LAB_PASSPHRASE CONFIG_LAB_PASSPHRASE
#endif
#if !defined(LAB_COUNTRY) && defined(CONFIG_LAB_COUNTRY)
#define LAB_COUNTRY CONFIG_LAB_COUNTRY
#endif
#if !defined(LAB_S1G_CHAN_NUM) && defined(CONFIG_LAB_S1G_CHAN_NUM)
#define LAB_S1G_CHAN_NUM CONFIG_LAB_S1G_CHAN_NUM
#endif
#if !defined(LAB_OP_CLASS) && defined(CONFIG_LAB_OP_CLASS)
#define LAB_OP_CLASS CONFIG_LAB_OP_CLASS
#endif
#if !defined(LAB_PRI_BW_MHZ) && defined(CONFIG_LAB_PRI_BW_MHZ)
#define LAB_PRI_BW_MHZ CONFIG_LAB_PRI_BW_MHZ
#endif
#if !defined(LAB_PRI_1MHZ_CHAN_IDX) && defined(CONFIG_LAB_PRI_1MHZ_CHAN_IDX)
#define LAB_PRI_1MHZ_CHAN_IDX CONFIG_LAB_PRI_1MHZ_CHAN_IDX
#endif
#if !defined(LAB_RELAY_AP_BSSID_STR) && defined(CONFIG_LAB_RELAY_AP_BSSID)
#define LAB_RELAY_AP_BSSID_STR CONFIG_LAB_RELAY_AP_BSSID
#endif
#if !defined(LAB_ROOT_AP_BSSID_STR) && defined(CONFIG_LAB_ROOT_AP_BSSID)
#define LAB_ROOT_AP_BSSID_STR CONFIG_LAB_ROOT_AP_BSSID
#endif
#if !defined(LAB_IP_NODE_B) && defined(CONFIG_LAB_IP_NODE_B)
#define LAB_IP_NODE_B CONFIG_LAB_IP_NODE_B
#endif
#if !defined(LAB_IP_RELAY) && defined(CONFIG_LAB_IP_RELAY)
#define LAB_IP_RELAY CONFIG_LAB_IP_RELAY
#endif
#if !defined(LAB_IP_NODE_A) && defined(CONFIG_LAB_IP_NODE_A)
#define LAB_IP_NODE_A CONFIG_LAB_IP_NODE_A
#endif
#if !defined(LAB_NETMASK) && defined(CONFIG_LAB_NETMASK)
#define LAB_NETMASK CONFIG_LAB_NETMASK
#endif
#if !defined(LAB_GATEWAY) && defined(CONFIG_LAB_GATEWAY)
#define LAB_GATEWAY CONFIG_LAB_GATEWAY
#endif
#if !defined(LAB_UDP_ECHO_PORT) && defined(CONFIG_LAB_UDP_ECHO_PORT)
#define LAB_UDP_ECHO_PORT CONFIG_LAB_UDP_ECHO_PORT
#endif
#if !defined(LAB_TCP_STREAM_PORT) && defined(CONFIG_LAB_TCP_STREAM_PORT)
#define LAB_TCP_STREAM_PORT CONFIG_LAB_TCP_STREAM_PORT
#endif
#if !defined(LAB_UDP_RATE_PPS) && defined(CONFIG_LAB_UDP_RATE_PPS)
#define LAB_UDP_RATE_PPS CONFIG_LAB_UDP_RATE_PPS
#endif
#if !defined(LAB_UDP_PAYLOAD_LEN) && defined(CONFIG_LAB_UDP_PAYLOAD_LEN)
#define LAB_UDP_PAYLOAD_LEN CONFIG_LAB_UDP_PAYLOAD_LEN
#endif
#if !defined(LAB_TCP_RATE_MPS) && defined(CONFIG_LAB_TCP_RATE_MPS)
#define LAB_TCP_RATE_MPS CONFIG_LAB_TCP_RATE_MPS
#endif
#if !defined(LAB_TCP_PAYLOAD_LEN) && defined(CONFIG_LAB_TCP_PAYLOAD_LEN)
#define LAB_TCP_PAYLOAD_LEN CONFIG_LAB_TCP_PAYLOAD_LEN
#endif
#if !defined(LAB_REPORT_INTERVAL_MS) && defined(CONFIG_LAB_REPORT_INTERVAL_MS)
#define LAB_REPORT_INTERVAL_MS CONFIG_LAB_REPORT_INTERVAL_MS
#endif
#if !defined(LAB_PING_INTERVAL_MS) && defined(CONFIG_LAB_PING_INTERVAL_MS)
#define LAB_PING_INTERVAL_MS CONFIG_LAB_PING_INTERVAL_MS
#endif

/* ---- Wi-Fi HaLow BSS ---------------------------------------------------------------------- */

/** SSID shared by the root AP (node B) and the relay AP. */
#ifndef LAB_SSID
#define LAB_SSID "halow-relay-lab"
#endif

/** SAE passphrase. Placeholder: MUST be changed for any deployment outside a shielded lab. */
#ifndef LAB_PASSPHRASE
#define LAB_PASSPHRASE "change-me-halow-au"
#endif

/** 1 = WPA3-SAE (the only security mode the lab uses). */
#ifndef LAB_SECURITY_SAE
#define LAB_SECURITY_SAE 1
#endif

/** 1 = Protected Management Frames required. */
#ifndef LAB_PMF_REQUIRED
#define LAB_PMF_REQUIRED 1
#endif

/** Regulatory domain. All nodes use the 802.11 REVmf AU channel list (SDK/esp-halow default). */
#ifndef LAB_COUNTRY
#define LAB_COUNTRY "AU"
#endif

/**
 * Operating channel: 2 MHz, S1G channel 37, centre 920.5 MHz. In the mm-iot-sdk 2.14.3 AU REVmf
 * list (s1g_channels_AU[23..48]) this is the entry {920500000, global op class 51, S1G op class
 * 23, chan 37, 2 MHz}. mmwlan_ap_args.op_class accepts either the global (51) or the S1G (23)
 * operating class together with s1g_chan_num.
 */
#ifndef LAB_S1G_CHAN_NUM
#define LAB_S1G_CHAN_NUM 37
#endif

/** Global operating class of the channel (the value passed as mmwlan_ap_args.op_class). */
#ifndef LAB_OP_CLASS
#define LAB_OP_CLASS 51
#endif

/** S1G operating class of the same channel (informational / alternative op_class value). */
#ifndef LAB_S1G_OP_CLASS
#define LAB_S1G_OP_CLASS 23
#endif

/** Operating channel bandwidth (MHz). */
#ifndef LAB_CHAN_BW_MHZ
#define LAB_CHAN_BW_MHZ 2
#endif

/** Centre frequency of the operating channel (Hz). */
#ifndef LAB_CENTRE_FREQ_HZ
#define LAB_CENTRE_FREQ_HZ 920500000UL
#endif

/** Primary channel bandwidth (MHz); 0 lets the SDK pick the widest allowed. */
#ifndef LAB_PRI_BW_MHZ
#define LAB_PRI_BW_MHZ 2
#endif

/** Index of the 1 MHz primary inside the operating channel (only used with 1 MHz primary). */
#ifndef LAB_PRI_1MHZ_CHAN_IDX
#define LAB_PRI_1MHZ_CHAN_IDX 0
#endif

/**
 * BSSID of the relay's AP interface (locally administered). Node A pins mmwlan_sta_args.bssid
 * to this so it can only associate through the relay, never directly to node B.
 */
#ifndef LAB_RELAY_AP_BSSID_STR
#define LAB_RELAY_AP_BSSID_STR "02:48:4C:00:00:02"
#endif

/** Same BSSID as a byte-array initialiser: uint8_t bssid[6] = LAB_RELAY_AP_BSSID_INIT; */
#ifndef LAB_RELAY_AP_BSSID_INIT
#define LAB_RELAY_AP_BSSID_INIT { 0x02, 0x48, 0x4C, 0x00, 0x00, 0x02 }
#endif

/** Optional BSSID the relay STA pins for the root AP; all-zero / empty means "any". */
#ifndef LAB_ROOT_AP_BSSID_STR
#define LAB_ROOT_AP_BSSID_STR ""
#endif

#ifndef LAB_ROOT_AP_BSSID_INIT
#define LAB_ROOT_AP_BSSID_INIT { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
#endif

/* ---- IPv4 plan: one L2 subnet bridged by the relay, static addresses, no DHCP --------------- */

#ifndef LAB_IP_NODE_B
#define LAB_IP_NODE_B "192.168.50.1"
#endif

#ifndef LAB_IP_RELAY
#define LAB_IP_RELAY "192.168.50.2"
#endif

#ifndef LAB_IP_NODE_A
#define LAB_IP_NODE_A "192.168.50.10"
#endif

#ifndef LAB_NETMASK
#define LAB_NETMASK "255.255.255.0"
#endif

/** No gateway: everything is on-link. */
#ifndef LAB_GATEWAY
#define LAB_GATEWAY "0.0.0.0"
#endif

/* ---- Traffic services --------------------------------------------------------------------- */

#ifndef LAB_UDP_ECHO_PORT
#define LAB_UDP_ECHO_PORT 5000
#endif

#ifndef LAB_TCP_STREAM_PORT
#define LAB_TCP_STREAM_PORT 5001
#endif

/** Throughput test sink ports on node B (ESP-IDF only, common/port/esp_idf/tput.h). */
#ifndef LAB_TPUT_TCP_PORT
#define LAB_TPUT_TCP_PORT 5002
#endif
#ifndef LAB_TPUT_UDP_PORT
#define LAB_TPUT_UDP_PORT 5003
#endif

/** UDP echo request rate (datagrams per second). */
#ifndef LAB_UDP_RATE_PPS
#define LAB_UDP_RATE_PPS 10
#endif

/** UDP echo application payload bytes (after the 32-byte linkproto header). */
#ifndef LAB_UDP_PAYLOAD_LEN
#define LAB_UDP_PAYLOAD_LEN 64
#endif

/** Grace period after the last UDP request to wait for late echoes. */
#ifndef LAB_UDP_DRAIN_MS
#define LAB_UDP_DRAIN_MS 2000
#endif

/** TCP stream message rate (messages per second). */
#ifndef LAB_TCP_RATE_MPS
#define LAB_TCP_RATE_MPS 5
#endif

/** TCP stream message payload bytes. */
#ifndef LAB_TCP_PAYLOAD_LEN
#define LAB_TCP_PAYLOAD_LEN 256
#endif

/** Oldest unacknowledged message older than this => stall => reconnect. */
#ifndef LAB_TCP_ACK_TIMEOUT_MS
#define LAB_TCP_ACK_TIMEOUT_MS 5000
#endif

#ifndef LAB_TCP_CONNECT_TIMEOUT_MS
#define LAB_TCP_CONNECT_TIMEOUT_MS 5000
#endif

#ifndef LAB_TCP_BACKOFF_MIN_MS
#define LAB_TCP_BACKOFF_MIN_MS 500
#endif

#ifndef LAB_TCP_BACKOFF_MAX_MS
#define LAB_TCP_BACKOFF_MAX_MS 30000
#endif

/** Messages the client may have outstanding (unacknowledged) at once. */
#ifndef LAB_TCP_MAX_INFLIGHT
#define LAB_TCP_MAX_INFLIGHT 32
#endif

/** Server closes a connection that delivered nothing for this long (0 = never). */
#ifndef LAB_TCP_SERVER_IDLE_TIMEOUT_MS
#define LAB_TCP_SERVER_IDLE_TIMEOUT_MS 60000
#endif

/**
 * Reconnect scan backoff of a HaLow STA (mmwlan_sta_args.scan_interval_base_s / _limit_s): the
 * wait between scans grows by the base factor up to the limit, and resets only on connection.
 * Used by the relay STA; node A's Kconfig defaults (LAB_A_SCAN_INTERVAL_*) match. The SDK
 * default limit of 512 s would keep a node off the air for minutes after its upstream AP returns.
 */
#ifndef LAB_STA_SCAN_INTERVAL_BASE_S
#define LAB_STA_SCAN_INTERVAL_BASE_S 2
#endif

#ifndef LAB_STA_SCAN_INTERVAL_LIMIT_S
#define LAB_STA_SCAN_INTERVAL_LIMIT_S 10
#endif

/** Interval between STATS lines. */
#ifndef LAB_REPORT_INTERVAL_MS
#define LAB_REPORT_INTERVAL_MS 5000
#endif

/** ICMP echo interval used by node A for A->B and A->relay pings. */
#ifndef LAB_PING_INTERVAL_MS
#define LAB_PING_INTERVAL_MS 1000
#endif

/** ICMP echo data size (bytes). */
#ifndef LAB_PING_SIZE
#define LAB_PING_SIZE 56
#endif

#endif /* HALOW_LAB_CONFIG_H */
