/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Node B: S1G relay root AP and lab traffic servers.
 *
 * Brings the HaLow interface up as the root of an mm-iot-sdk S1G relay network (static IPv4, no
 * DHCP), reports station and relay status as EVENT lines, and runs the UDP echo server and the
 * TCP message stream server from common/ for node A, which reaches the root through the relay.
 *
 * B only answers traffic, so it learns peers' ARP entries from their requests; that keeps it
 * working even where the root can not send group-addressed frames (issue #10).
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"

#include "halow_lab_config.h"
#include "lab_platform.h"
#include "lab_services.h"
#include "node_util.h"
#include "ping_probe.h"
#include "tput.h"

#ifndef CONFIG_HALOW_RELAY_MODE
#error "node_b needs CONFIG_HALOW_AP_MODE=y and CONFIG_HALOW_RELAY_MODE=y (see sdkconfig.defaults)"
#endif

#if defined(CONFIG_WIFI_MORSE_AU_CHANNELISATION_2020) || \
    defined(CONFIG_WIFI_MORSE_AU_CHANNELISATION_2024)
#error \
    "All lab nodes use the AU 802.11 REVmf channel list (CONFIG_WIFI_MORSE_AU_CHANNELISATION_REVMF)"
#endif

_Static_assert(sizeof(LAB_SSID) - 1 > 0 && sizeof(LAB_SSID) - 1 <= MMWLAN_SSID_MAXLEN,
               "LAB_SSID must be 1..32 bytes");
_Static_assert(sizeof(LAB_PASSPHRASE) - 1 > 0 &&
                   sizeof(LAB_PASSPHRASE) - 1 <= MMWLAN_PASSPHRASE_MAXLEN,
               "LAB_PASSPHRASE must be 1..100 bytes");

static const char *TAG = "node_b";

/* How often app_main checks the netif link state. */
#define LINK_POLL_MS 200

/* Stack for each traffic service task (lab_services.h recommends 6 KiB). */
#define SVC_STACK_BYTES 6144

/* Delay before restarting a service whose *_run() returned an error. */
#define SVC_RETRY_MS 2000

/* Stations tracked for the health line; the root allows at most LAB_B_MAX_STAS (<= 20). */
#define STA_TABLE_SIZE 20

#if CONFIG_LAB_B_PING_RELAY
#define PING_RELAY_ENABLED 1
#else
#define PING_RELAY_ENABLED 0
#endif
#if CONFIG_LAB_B_UDP_ECHO_RELAY
#define UDP_RELAY_ENABLED 1
#else
#define UDP_RELAY_ENABLED 0
#endif
#define RELAY_PROBES_ENABLED (PING_RELAY_ENABLED || UDP_RELAY_ENABLED)

#if CONFIG_LAB_B_TPUT_CLIENT
static void start_tput_client(void);
#endif

#if CONFIG_LAB_B_STATIC_ARP
#define STATIC_ARP_ENABLED 1
#else
#define STATIC_ARP_ENABLED 0
#endif

enum node_event_type
{
    EVENT_RELAY_STATUS,
    EVENT_AP_STA_STATUS,
};

struct node_event
{
    enum node_event_type type;

    union
    {
        struct mmwlan_relay_status relay;
        struct mmwlan_ap_sta_status sta;
    };
};

struct sta_entry
{
    bool used;
    uint8_t mac[NODE_MAC_LEN];
    uint16_t aid;
    enum mmwlan_ap_sta_state state;
};

/* morselib callbacks must not block or call mmwlan, so they only queue events for app_main. */
static QueueHandle_t event_queue;
static volatile uint32_t event_queue_drops;

static struct sta_entry sta_table[STA_TABLE_SIZE];

/* Kept for the lifetime of the AP: sta_status_cb_arg must stay valid. */
static struct mmwlan_ap_args root_ap_args = MMWLAN_AP_ARGS_INIT;

static void queue_event(const struct node_event *evt)
{
    if (xQueueSend(event_queue, evt, 0) != pdTRUE)
    {
        event_queue_drops++;
    }
}

static void relay_status_cb(const struct mmwlan_relay_status *status, void *arg)
{
    struct node_event evt = { .type = EVENT_RELAY_STATUS, .relay = *status };
    (void)arg;
    queue_event(&evt);
}

static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *sta_status, void *arg)
{
    struct node_event evt = { .type = EVENT_AP_STA_STATUS, .sta = *sta_status };
    (void)arg;
    queue_event(&evt);
}

static const char *relay_state_str(enum mmwlan_relay_state state)
{
    switch (state)
    {
        case MMWLAN_RELAY_STATE_DISABLED:
            return "disabled";

        case MMWLAN_RELAY_STATE_ROOT:
            return "root";

        case MMWLAN_RELAY_STATE_STA:
            return "station";

        case MMWLAN_RELAY_STATE_REPEATER:
            return "repeater";
    }
    return "unknown";
}

static const char *ap_sta_state_str(enum mmwlan_ap_sta_state state)
{
    switch (state)
    {
        case MMWLAN_AP_STA_UNKNOWN:
            return "gone";

        case MMWLAN_AP_STA_AUTHENTICATED:
            return "authenticated";

        case MMWLAN_AP_STA_ASSOCIATED:
            return "associated";

        case MMWLAN_AP_STA_AUTHORIZED:
            return "authorized";
    }
    return "unknown";
}

static void sta_table_update(const struct mmwlan_ap_sta_status *sta)
{
    struct sta_entry *free_slot = NULL;

    for (int ii = 0; ii < STA_TABLE_SIZE; ii++)
    {
        struct sta_entry *entry = &sta_table[ii];
        if (!entry->used)
        {
            if (free_slot == NULL)
            {
                free_slot = entry;
            }
            continue;
        }
        if (node_mac_equal(entry->mac, sta->mac_addr))
        {
            if (sta->state == MMWLAN_AP_STA_UNKNOWN)
            {
                entry->used = false;
            }
            else
            {
                entry->aid = sta->aid;
                entry->state = sta->state;
            }
            return;
        }
    }

    if (sta->state != MMWLAN_AP_STA_UNKNOWN && free_slot != NULL)
    {
        free_slot->used = true;
        memcpy(free_slot->mac, sta->mac_addr, NODE_MAC_LEN);
        free_slot->aid = sta->aid;
        free_slot->state = sta->state;
    }
}

static unsigned sta_table_count(bool authorized_only)
{
    unsigned count = 0;

    for (int ii = 0; ii < STA_TABLE_SIZE; ii++)
    {
        if (sta_table[ii].used &&
            (!authorized_only || sta_table[ii].state == MMWLAN_AP_STA_AUTHORIZED))
        {
            count++;
        }
    }
    return count;
}

static void install_static_arp(bool quiet)
{
#if CONFIG_LAB_B_STATIC_ARP
    node_static_arp_set("node_a", LAB_IP_NODE_A, CONFIG_LAB_B_STATIC_ARP_NODE_A_MAC, quiet);
    node_static_arp_set("relay", LAB_IP_RELAY, CONFIG_LAB_B_STATIC_ARP_RELAY_MAC, quiet);
#else
    (void)quiet;
#endif
}

static void log_relay_status(const struct mmwlan_relay_status *status)
{
    char depth[8];

    if (status->depth == MMWLAN_RELAY_DEPTH_UNKNOWN)
    {
        snprintf(depth, sizeof(depth), "unknown");
    }
    else
    {
        snprintf(depth, sizeof(depth), "%u", (unsigned)status->depth);
    }
    lab_outf("EVENT,relay,t_ms=%" PRIu32 ",ev=status,state=%s,depth=%s",
             node_uptime_ms(),
             relay_state_str(status->state),
             depth);
}

#if RELAY_PROBES_ENABLED
static void start_relay_probes(void);
#endif

static void handle_event(const struct node_event *evt, bool link_up)
{
    char mac[NODE_MAC_STR_LEN];

    switch (evt->type)
    {
        case EVENT_RELAY_STATUS:
            log_relay_status(&evt->relay);
            break;

        case EVENT_AP_STA_STATUS:
            sta_table_update(&evt->sta);
            node_format_mac(evt->sta.mac_addr, mac);
            lab_outf("EVENT,ap,t_ms=%" PRIu32 ",ev=sta,mac=%s,aid=%u,state=%s,stas=%u",
                     node_uptime_ms(),
                     mac,
                     (unsigned)evt->sta.aid,
                     ap_sta_state_str(evt->sta.state),
                     sta_table_count(true));
            /* A newly authorized station must not leave an existing peer without an entry. */
            if (evt->sta.state == MMWLAN_AP_STA_AUTHORIZED && link_up)
            {
                install_static_arp(false);
            }
#if RELAY_PROBES_ENABLED
            if (evt->sta.state == MMWLAN_AP_STA_AUTHORIZED)
            {
                start_relay_probes();
            }
#endif
#if CONFIG_LAB_B_TPUT_CLIENT
            if (evt->sta.state == MMWLAN_AP_STA_AUTHORIZED)
            {
                start_tput_client();
            }
#endif
            break;
    }
}

struct svc_slot
{
    const char *name;
    int (*run)(void);
};

#if CONFIG_LAB_B_UDP_ECHO_SERVER
static int run_udp_echo_server(void)
{
    struct udp_echo_server_cfg cfg;

    udp_echo_server_cfg_init(&cfg);
    cfg.port = LAB_UDP_ECHO_PORT;
    cfg.report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg.stop = NULL;
    return udp_echo_server_run(&cfg);
}
#endif

#if CONFIG_LAB_B_TCP_STREAM_SERVER
static int run_tcp_stream_server(void)
{
    struct tcp_stream_server_cfg cfg;

    tcp_stream_server_cfg_init(&cfg);
    cfg.port = LAB_TCP_STREAM_PORT;
    cfg.report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg.idle_timeout_ms = CONFIG_LAB_B_TCP_IDLE_TIMEOUT_MS;
    cfg.stop = NULL;
    return tcp_stream_server_run(&cfg);
}
#endif

/* The servers bind INADDR_ANY and run for the life of the node, across link changes. */
static void svc_task(void *arg)
{
    const struct svc_slot *slot = (const struct svc_slot *)arg;

    while (true)
    {
        int rc = slot->run();
        lab_outf("EVENT,svc,t_ms=%" PRIu32 ",ev=exit,name=%s,rc=%d,retry_ms=%u",
                 node_uptime_ms(),
                 slot->name,
                 rc,
                 (unsigned)SVC_RETRY_MS);
        lab_sleep_ms(SVC_RETRY_MS);
    }
}

static void start_servers(void)
{
    static const struct svc_slot slots[] = {
#if CONFIG_LAB_B_UDP_ECHO_SERVER
        { "udp_srv", run_udp_echo_server },
#endif
#if CONFIG_LAB_B_TCP_STREAM_SERVER
        { "tcp_srv", run_tcp_stream_server },
#endif
        { NULL, NULL },
    };

    for (const struct svc_slot *slot = slots; slot->name != NULL; slot++)
    {
        if (lab_task_create(svc_task,
                            (void *)slot,
                            slot->name,
                            SVC_STACK_BYTES,
                            LAB_TASK_PRIO_NORMAL) != 0)
        {
            ESP_LOGE(TAG, "Unable to start the %s task", slot->name);
        }
    }
}

#if RELAY_PROBES_ENABLED
#if CONFIG_LAB_B_PING_RELAY
static struct ping_probe relay_ping = {
    .name = "relay",
    .dst = LAB_IP_RELAY,
    .interval_ms = CONFIG_LAB_B_PING_INTERVAL_MS,
    .timeout_ms = CONFIG_LAB_B_PING_TIMEOUT_MS,
    .data_size = 56,
    .report_interval_ms = LAB_REPORT_INTERVAL_MS,
    .verbose = true,
};
#endif

#if CONFIG_LAB_B_UDP_ECHO_RELAY
static int run_udp_echo_relay(void)
{
    struct udp_echo_client_cfg cfg;

    udp_echo_client_cfg_init(&cfg);
    cfg.dst_ip = LAB_IP_RELAY;
    cfg.dst_port = LAB_UDP_ECHO_PORT;
    cfg.rate_pps = LAB_UDP_RATE_PPS;
    cfg.payload_len = LAB_UDP_PAYLOAD_LEN;
    cfg.report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg.count = 0;
    cfg.duration_ms = 0;
    cfg.flow = "udp_relay";
    cfg.stop = NULL;
    return udp_echo_client_run(&cfg, NULL);
}
#endif

/* Diagnostic root -> relay traffic, started once, when the first station is authorized. */
static void start_relay_probes(void)
{
    static bool started;

    if (started)
    {
        return;
    }
    started = true;

#if CONFIG_LAB_B_PING_RELAY
    if (ping_probe_start(&relay_ping) != ESP_OK)
    {
        ESP_LOGE(TAG, "Unable to start pinging the relay");
    }
#endif
#if CONFIG_LAB_B_UDP_ECHO_RELAY
    static const struct svc_slot slot = { "udp_relay", run_udp_echo_relay };
    if (lab_task_create(svc_task, (void *)&slot, slot.name, SVC_STACK_BYTES,
                        LAB_TASK_PRIO_NORMAL) != 0)
    {
        ESP_LOGE(TAG, "Unable to start the %s task", slot.name);
    }
#endif
    lab_outf("EVENT,svc,t_ms=%" PRIu32 ",ev=start,ping_relay=%d,udp_relay=%d,dst=%s",
             node_uptime_ms(),
             PING_RELAY_ENABLED,
             UDP_RELAY_ENABLED,
             LAB_IP_RELAY);
}
#endif

#if CONFIG_LAB_B_TPUT_CLIENT
static void tput_client_task(void *arg)
{
    (void)arg;
    tput_client_loop(LAB_IP_NODE_A, CONFIG_LAB_B_TPUT_DURATION_MS, CONFIG_LAB_B_TPUT_PAUSE_MS,
                     CONFIG_LAB_B_TPUT_UDP_LEN, NULL);
}

/* B -> A throughput client, started once, when the first station is authorized. */
static void start_tput_client(void)
{
    static bool started;

    if (started)
    {
        return;
    }
    started = true;
    if (lab_task_create(tput_client_task, NULL, "tput_cli", SVC_STACK_BYTES,
                        LAB_TASK_PRIO_NORMAL) != 0)
    {
        ESP_LOGE(TAG, "Unable to start the throughput client");
    }
}
#endif

static void halt(const char *why)
{
    lab_outf("EVENT,node,t_ms=%" PRIu32 ",ev=fatal,reason=%s", node_uptime_ms(), why);
    while (true)
    {
        ESP_LOGE(TAG, "Halted: %s", why);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void start_root_ap(void)
{
    struct mmwlan_relay_args relay_args = MMWLAN_RELAY_ARGS_INIT;
    uint8_t root_bssid[NODE_MAC_LEN];

    memcpy(root_ap_args.ssid, LAB_SSID, strlen(LAB_SSID));
    root_ap_args.ssid_len = strlen(LAB_SSID);
    root_ap_args.security_type = MMWLAN_SAE;
    memcpy(root_ap_args.passphrase, LAB_PASSPHRASE, strlen(LAB_PASSPHRASE));
    /* Always explicit (issue #11 is in the STA path, but do not rely on the default). */
    root_ap_args.passphrase_len = strlen(LAB_PASSPHRASE);
    root_ap_args.pmf_mode = LAB_PMF_REQUIRED ? MMWLAN_PMF_REQUIRED : MMWLAN_PMF_DISABLED;
    root_ap_args.op_class = LAB_OP_CLASS;
    root_ap_args.s1g_chan_num = LAB_S1G_CHAN_NUM;
    root_ap_args.pri_bw_mhz = LAB_PRI_BW_MHZ;
    root_ap_args.pri_1mhz_chan_idx = LAB_PRI_1MHZ_CHAN_IDX;
    root_ap_args.max_stas = CONFIG_LAB_B_MAX_STAS;
    root_ap_args.sta_status_cb = ap_sta_status_cb;
    root_ap_args.sta_status_cb_arg = NULL;

    if (LAB_ROOT_AP_BSSID_STR[0] != '\0')
    {
        if (!node_parse_mac(LAB_ROOT_AP_BSSID_STR, root_bssid))
        {
            halt("bad_root_bssid");
        }
        memcpy(root_ap_args.bssid, root_bssid, NODE_MAC_LEN);
    }

    relay_args.forwarding_table_size = CONFIG_LAB_B_FORWARDING_TABLE_SIZE;
    relay_args.root_ap_args = &root_ap_args;

    if (mmwlan_register_relay_status_cb(relay_status_cb, NULL) != MMWLAN_SUCCESS)
    {
        ESP_LOGW(TAG, "Unable to register the relay status callback");
    }

    ESP_LOGI(TAG,
             "Starting relay root AP \"%s\" (SAE, PMF %s) on S1G channel %d, op class %d",
             LAB_SSID,
             LAB_PMF_REQUIRED ? "required" : "disabled",
             LAB_S1G_CHAN_NUM,
             LAB_OP_CLASS);

    esp_err_t err = mmhalow_relay_enable(&relay_args);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mmhalow_relay_enable failed: %s", esp_err_to_name(err));
        halt("relay_enable_failed");
    }

    char bssid_str[NODE_MAC_STR_LEN] = "unknown";
    uint8_t bssid[NODE_MAC_LEN];
    if (mmwlan_ap_get_bssid(bssid) == MMWLAN_SUCCESS)
    {
        node_format_mac(bssid, bssid_str);
    }

    struct mmwlan_vif_channel_info chan = { 0 };
    if (mmwlan_get_vif_channel_info(MMWLAN_VIF_AP, &chan) != MMWLAN_SUCCESS)
    {
        chan.op_class = LAB_OP_CLASS;
        chan.s1g_chan_num = LAB_S1G_CHAN_NUM;
    }

    lab_outf("EVENT,wlan,t_ms=%" PRIu32 ",ev=root_up,bssid=%s,ssid=%s,chan=%u,op_class=%u,"
             "pri_bw_mhz=%u,max_stas=%u",
             node_uptime_ms(),
             bssid_str,
             LAB_SSID,
             (unsigned)chan.s1g_chan_num,
             (unsigned)chan.op_class,
             (unsigned)chan.pri_bw_mhz,
             (unsigned)CONFIG_LAB_B_MAX_STAS);
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    lab_platform_init();

    event_queue = xQueueCreate(16, sizeof(struct node_event));
    assert(event_queue != NULL);

    if (strcmp(LAB_COUNTRY, CONFIG_HALOW_COUNTRY_CODE) != 0)
    {
        ESP_LOGE(TAG,
                 "CONFIG_LAB_COUNTRY \"%s\" differs from CONFIG_HALOW_COUNTRY_CODE \"%s\"",
                 LAB_COUNTRY,
                 CONFIG_HALOW_COUNTRY_CODE);
        halt("country_mismatch");
    }

    ESP_ERROR_CHECK(mmhalow_init(NULL));
    esp_netif_t *netif = mmhalow_get_netif();
    assert(netif != NULL);

    esp_err_t err = node_set_static_ip(netif, LAB_IP_NODE_B, LAB_NETMASK, LAB_GATEWAY);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Static IP setup failed: %s", esp_err_to_name(err));
        halt("static_ip_failed");
    }

    uint8_t mac[NODE_MAC_LEN] = { 0 };
    char mac_str[NODE_MAC_STR_LEN];
    (void)esp_netif_get_mac(netif, mac);
    node_format_mac(mac, mac_str);
    lab_outf("EVENT,node,t_ms=%" PRIu32 ",ev=boot,app=node_b,role=root,mac=%s,ip=%s,netmask=%s,"
             "country=%s,static_arp=%d",
             node_uptime_ms(),
             mac_str,
             LAB_IP_NODE_B,
             LAB_NETMASK,
             LAB_COUNTRY,
             STATIC_ARP_ENABLED);

    start_root_ap();
    start_servers();
#if CONFIG_LAB_B_TPUT_SERVER
    struct tput_server_cfg tput_cfg = {
        .tcp_port = LAB_TPUT_TCP_PORT,
        .udp_port = LAB_TPUT_UDP_PORT,
        .report_interval_ms = 1000,
    };
    if (tput_server_start(&tput_cfg) != 0)
    {
        ESP_LOGE(TAG, "Unable to start the throughput sink");
    }
#endif

    bool link_up = false;
    uint32_t link_change_ms = node_uptime_ms();
    uint32_t next_health_ms = node_uptime_ms() + CONFIG_LAB_B_HEALTH_INTERVAL_MS;
    uint32_t reported_drops = 0;

    while (true)
    {
        struct node_event evt;
        if (xQueueReceive(event_queue, &evt, pdMS_TO_TICKS(LINK_POLL_MS)) == pdTRUE)
        {
            handle_event(&evt, link_up);
        }

        uint32_t now = node_uptime_ms();

        /* The netif link follows the root AP VIF (esp-halow drives it from morselib). */
        bool up = esp_netif_is_netif_up(netif);
        if (up != link_up)
        {
            lab_outf("EVENT,wlan,t_ms=%" PRIu32 ",ev=%s,%s_ms=%" PRIu32,
                     now,
                     up ? "link_up" : "link_down",
                     up ? "down" : "up",
                     now - link_change_ms);
            link_up = up;
            link_change_ms = now;
            if (up)
            {
                install_static_arp(false);
            }
        }

        if (node_time_reached(now, next_health_ms))
        {
            char extra[96];
            uint32_t drops = event_queue_drops;

            next_health_ms = now + CONFIG_LAB_B_HEALTH_INTERVAL_MS;
            snprintf(extra,
                     sizeof(extra),
                     ",role=root,link=%d,stas=%u,stas_known=%u,evt_drops=%" PRIu32,
                     link_up ? 1 : 0,
                     sta_table_count(true),
                     sta_table_count(false),
                     drops);
            node_log_health(extra);
            if (drops != reported_drops)
            {
                ESP_LOGW(TAG, "%" PRIu32 " morselib events dropped (queue full)", drops);
                reported_drops = drops;
            }
            if (link_up)
            {
                install_static_arp(true);
            }
        }
    }
}
