/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Node A: HaLow STA that reaches node B through the relay.
 *
 * Joins the lab BSS with a static IPv4 address, pinned by default to the relay's AP BSSID so it
 * can not associate with the root directly. Once the link is up it runs the lab traffic: UDP echo
 * client and TCP message stream client to node B (common/ lab_services), and ICMP pings to node
 * B and the relay. Services are stopped on link down and restarted on link up.
 *
 * app_main is the supervisor: it polls the netif link state (which esp-halow drives from
 * morselib), and logs STA and relay events that morselib callbacks queue for it.
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
#include "freertos/event_groups.h"
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

#if CONFIG_A_MODE_RELAY_STA && !defined(CONFIG_HALOW_RELAY_MODE)
#error "A_MODE_RELAY_STA needs CONFIG_HALOW_AP_MODE=y and CONFIG_HALOW_RELAY_MODE=y"
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

static const char *TAG = "node_a";

#if CONFIG_A_MODE_RELAY_STA
#define MODE_NAME "relay_sta"
#else
#define MODE_NAME "plain_sta"
#endif

#if CONFIG_LAB_A_PIN_RELAY
#define PIN_NAME  "relay"
#define PIN_BSSID LAB_RELAY_AP_BSSID_STR
#elif CONFIG_LAB_A_PIN_ROOT
#define PIN_NAME  "root"
#define PIN_BSSID LAB_ROOT_AP_BSSID_STR
#else
#define PIN_NAME  "any"
#define PIN_BSSID "none"
#endif

#if CONFIG_LAB_A_STOP_SERVICES_ON_LINK_DOWN
#define STOP_SERVICES_ON_LINK_DOWN 1
#else
#define STOP_SERVICES_ON_LINK_DOWN 0
#endif

#ifdef CONFIG_LAB_A_STATIC_ARP_NODE_B_MAC
#define STATIC_ARP_NODE_B_MAC CONFIG_LAB_A_STATIC_ARP_NODE_B_MAC
#else
#define STATIC_ARP_NODE_B_MAC ""
#endif

#ifdef CONFIG_LAB_A_STATIC_ARP_RELAY_MAC
#define STATIC_ARP_RELAY_MAC CONFIG_LAB_A_STATIC_ARP_RELAY_MAC
#else
#define STATIC_ARP_RELAY_MAC ""
#endif

/* Supervisor poll period (link state, service shutdown, timers). */
#define SUPERVISOR_POLL_MS 100

/* Stack for each traffic service task (lab_services.h recommends 6 KiB). */
#define SVC_STACK_BYTES 6144

/* Delay before restarting a service whose *_run() returned an error. */
#define SVC_RETRY_MS 2000

/* How long a stop waits for the services before carrying on (they poll every 100 ms). */
#define SVC_STOP_WAIT_MS 3000

/* Retry period when the STA or relay can not be enabled at all. */
#define WLAN_START_RETRY_MS 5000

enum node_event_type
{
    EVENT_STA_STATE,
    EVENT_RELAY_STATUS,
};

struct node_event
{
    enum node_event_type type;

    union
    {
        enum mmwlan_sta_state sta_state;
        struct mmwlan_relay_status relay;
    };
};

/* morselib callbacks must not block or call mmwlan, so they only queue events for app_main. */
static QueueHandle_t event_queue;
static volatile uint32_t event_queue_drops;

static esp_netif_t *halow_netif;

static struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;

/* ---- Traffic services --------------------------------------------------------------------- */

enum svc_state
{
    SVC_IDLE,
    SVC_RUNNING,
    SVC_STOPPING,
};

struct svc_slot
{
    const char *name;
    int (*run)(void);
    bool enabled;
};

static volatile bool svc_stop;
static EventGroupHandle_t svc_done;
static EventBits_t svc_started_bits;
static enum svc_state svc_state = SVC_IDLE;
static uint32_t svc_stop_started_ms;

static int run_udp_echo(const char *dst_ip, const char *flow)
{
    struct udp_echo_client_cfg cfg;

    udp_echo_client_cfg_init(&cfg);
    cfg.dst_ip = dst_ip;
    cfg.dst_port = LAB_UDP_ECHO_PORT;
    cfg.rate_pps = LAB_UDP_RATE_PPS;
    cfg.payload_len = LAB_UDP_PAYLOAD_LEN;
    cfg.report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg.count = 0;
    cfg.duration_ms = 0;
    cfg.flow = flow;
    cfg.stop = &svc_stop;
    return udp_echo_client_run(&cfg, NULL);
}

static int run_udp_echo_node_b(void)
{
    return run_udp_echo(LAB_IP_NODE_B, "udp");
}

static int run_udp_echo_relay(void)
{
    return run_udp_echo(LAB_IP_RELAY, "udp_relay");
}

static int run_tcp_stream(void)
{
    struct tcp_stream_client_cfg cfg;

    tcp_stream_client_cfg_init(&cfg);
    cfg.dst_ip = LAB_IP_NODE_B;
    cfg.dst_port = LAB_TCP_STREAM_PORT;
    cfg.rate_mps = LAB_TCP_RATE_MPS;
    cfg.payload_len = LAB_TCP_PAYLOAD_LEN;
    cfg.report_interval_ms = LAB_REPORT_INTERVAL_MS;
    cfg.count = 0;
    cfg.duration_ms = 0;
    cfg.connect_timeout_ms = CONFIG_LAB_A_TCP_CONNECT_TIMEOUT_MS;
    cfg.ack_timeout_ms = CONFIG_LAB_A_TCP_ACK_TIMEOUT_MS;
    cfg.backoff_min_ms = CONFIG_LAB_A_TCP_BACKOFF_MIN_MS;
    cfg.backoff_max_ms = CONFIG_LAB_A_TCP_BACKOFF_MAX_MS;
    cfg.max_inflight = CONFIG_LAB_A_TCP_MAX_INFLIGHT;
    cfg.flow = "tcp";
    cfg.stop = &svc_stop;
    return tcp_stream_client_run(&cfg, NULL);
}

#if CONFIG_LAB_A_TPUT
/* Alternating TCP and UDP bulk tests to node B until the services are stopped. */
static int run_tput(void)
{
    tput_client_loop(LAB_IP_NODE_B, CONFIG_LAB_A_TPUT_DURATION_MS, CONFIG_LAB_A_TPUT_PAUSE_MS,
                     CONFIG_LAB_A_TPUT_UDP_LEN, &svc_stop);
    return LAB_SVC_OK;
}
#define SVC_TPUT_ENABLED true
#else
static int run_tput(void)
{
    return LAB_SVC_OK;
}
#define SVC_TPUT_ENABLED false
#endif

#if CONFIG_LAB_A_UDP_ECHO_NODE_B
#define SVC_UDP_NODE_B_ENABLED true
#else
#define SVC_UDP_NODE_B_ENABLED false
#endif
#if CONFIG_LAB_A_UDP_ECHO_RELAY
#define SVC_UDP_RELAY_ENABLED true
#else
#define SVC_UDP_RELAY_ENABLED false
#endif
#if CONFIG_LAB_A_TCP_STREAM
#define SVC_TCP_ENABLED true
#else
#define SVC_TCP_ENABLED false
#endif

static struct svc_slot svc_slots[] = {
    { "udp", run_udp_echo_node_b, SVC_UDP_NODE_B_ENABLED },
    { "udp_relay", run_udp_echo_relay, SVC_UDP_RELAY_ENABLED },
    { "tcp", run_tcp_stream, SVC_TCP_ENABLED },
    { "tput", run_tput, SVC_TPUT_ENABLED },
};

#define SVC_COUNT    (sizeof(svc_slots) / sizeof(svc_slots[0]))
#define SVC_ALL_BITS ((EventBits_t)((1u << SVC_COUNT) - 1))

#if CONFIG_LAB_A_PING_VERBOSE
#define PING_VERBOSE true
#else
#define PING_VERBOSE false
#endif
#if CONFIG_LAB_A_PING_NODE_B
#define PING_NODE_B_ENABLED 1
#else
#define PING_NODE_B_ENABLED 0
#endif
#if CONFIG_LAB_A_PING_RELAY
#define PING_RELAY_ENABLED 1
#else
#define PING_RELAY_ENABLED 0
#endif

static struct ping_probe ping_probes[] = {
#if CONFIG_LAB_A_PING_NODE_B
    {
        .name = "node_b",
        .dst = LAB_IP_NODE_B,
        .interval_ms = LAB_PING_INTERVAL_MS,
        .timeout_ms = CONFIG_LAB_A_PING_TIMEOUT_MS,
        .data_size = CONFIG_LAB_A_PING_SIZE,
        .report_interval_ms = LAB_REPORT_INTERVAL_MS,
        .verbose = PING_VERBOSE,
    },
#endif
#if CONFIG_LAB_A_PING_RELAY
    {
        .name = "relay",
        .dst = LAB_IP_RELAY,
        .interval_ms = LAB_PING_INTERVAL_MS,
        .timeout_ms = CONFIG_LAB_A_PING_TIMEOUT_MS,
        .data_size = CONFIG_LAB_A_PING_SIZE,
        .report_interval_ms = LAB_REPORT_INTERVAL_MS,
        .verbose = PING_VERBOSE,
    },
#endif
    /* Placeholder so the array is never empty; skipped because name is NULL. */
    { .name = NULL },
};

static void svc_task(void *arg)
{
    const struct svc_slot *slot = (const struct svc_slot *)arg;
    EventBits_t bit = (EventBits_t)(1u << (slot - svc_slots));

    while (!svc_stop)
    {
        int rc = slot->run();
        if (svc_stop || rc == LAB_SVC_OK)
        {
            break;
        }
        lab_outf("EVENT,svc,t_ms=%" PRIu32 ",ev=exit,name=%s,rc=%d,retry_ms=%u",
                 node_uptime_ms(),
                 slot->name,
                 rc,
                 (unsigned)SVC_RETRY_MS);
        for (uint32_t waited = 0; waited < SVC_RETRY_MS && !svc_stop; waited += LAB_SVC_POLL_MS)
        {
            lab_sleep_ms(LAB_SVC_POLL_MS);
        }
    }
    xEventGroupSetBits(svc_done, bit);
}

static void services_start(void)
{
    svc_stop = false;
    svc_started_bits = 0;

    for (size_t ii = 0; ii < SVC_COUNT; ii++)
    {
        struct svc_slot *slot = &svc_slots[ii];
        EventBits_t bit = (EventBits_t)(1u << ii);

        if (!slot->enabled)
        {
            continue;
        }
        xEventGroupClearBits(svc_done, bit);
        if (lab_task_create(svc_task, slot, slot->name, SVC_STACK_BYTES, LAB_TASK_PRIO_NORMAL) != 0)
        {
            ESP_LOGE(TAG, "Unable to start the %s task", slot->name);
            xEventGroupSetBits(svc_done, bit);
            continue;
        }
        svc_started_bits |= bit;
    }

    for (struct ping_probe *probe = ping_probes; probe->name != NULL; probe++)
    {
        if (ping_probe_start(probe) != ESP_OK)
        {
            ESP_LOGE(TAG, "Unable to start pinging %s", probe->name);
        }
    }

    svc_state = SVC_RUNNING;
    lab_outf("EVENT,svc,t_ms=%" PRIu32 ",ev=start,udp=%d,udp_relay=%d,tcp=%d,ping_node_b=%d,"
             "ping_relay=%d",
             node_uptime_ms(),
             SVC_UDP_NODE_B_ENABLED ? 1 : 0,
             SVC_UDP_RELAY_ENABLED ? 1 : 0,
             SVC_TCP_ENABLED ? 1 : 0,
             PING_NODE_B_ENABLED,
             PING_RELAY_ENABLED);
}

static bool pings_all_done(TickType_t wait)
{
    TickType_t start = xTaskGetTickCount();

    for (;;)
    {
        bool done = true;
        for (struct ping_probe *probe = ping_probes; probe->name != NULL; probe++)
        {
            done = done && !ping_probe_is_active(probe);
        }
        if (done || (xTaskGetTickCount() - start) >= wait)
        {
            return done;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Every service task and every ping probe task has exited (waits up to @p wait for each). */
static bool services_all_done(TickType_t wait)
{
    bool svc_done_all = true;
    if (svc_started_bits != 0)
    {
        EventBits_t bits = xEventGroupWaitBits(svc_done, svc_started_bits, pdFALSE, pdTRUE, wait);
        svc_done_all = (bits & svc_started_bits) == svc_started_bits;
    }
    return pings_all_done(wait) && svc_done_all;
}

static void services_finish_stop(void)
{
    lab_outf("EVENT,svc,t_ms=%" PRIu32 ",ev=stopped,wait_ms=%" PRIu32,
             node_uptime_ms(),
             node_uptime_ms() - svc_stop_started_ms);
    svc_started_bits = 0;
    svc_stop = false;
    svc_state = SVC_IDLE;
}

static void services_stop(const char *reason)
{
    if (svc_state != SVC_RUNNING)
    {
        return;
    }

    svc_stop_started_ms = node_uptime_ms();
    lab_outf("EVENT,svc,t_ms=%" PRIu32 ",ev=stop,reason=%s", svc_stop_started_ms, reason);
    svc_stop = true;
    for (struct ping_probe *probe = ping_probes; probe->name != NULL; probe++)
    {
        ping_probe_stop(probe);
    }

    if (services_all_done(pdMS_TO_TICKS(SVC_STOP_WAIT_MS)))
    {
        services_finish_stop();
    }
    else
    {
        /* Not restarted until every task has exited; supervisor_poll_services() keeps checking. */
        ESP_LOGW(TAG, "Services still running %d ms after stop", SVC_STOP_WAIT_MS);
        svc_state = SVC_STOPPING;
    }
}

static void supervisor_poll_services(void)
{
    if (svc_state == SVC_STOPPING && services_all_done(0))
    {
        services_finish_stop();
    }
}

/* ---- WLAN --------------------------------------------------------------------------------- */

static void queue_event(const struct node_event *evt)
{
    if (xQueueSend(event_queue, evt, 0) != pdTRUE)
    {
        event_queue_drops++;
    }
}

static void sta_status_cb(enum mmwlan_sta_state sta_state)
{
    struct node_event evt = { .type = EVENT_STA_STATE, .sta_state = sta_state };
    queue_event(&evt);
}

#if CONFIG_A_MODE_RELAY_STA
static void relay_status_cb(const struct mmwlan_relay_status *status, void *arg)
{
    struct node_event evt = { .type = EVENT_RELAY_STATUS, .relay = *status };
    (void)arg;
    queue_event(&evt);
}
#endif

static const char *sta_state_str(enum mmwlan_sta_state state)
{
    switch (state)
    {
        case MMWLAN_STA_DISABLED:
            return "disabled";

        case MMWLAN_STA_CONNECTING:
            return "connecting";

        case MMWLAN_STA_CONNECTED:
            return "connected";
    }
    return "unknown";
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

static void format_depth(uint8_t depth, char *buf, size_t len)
{
    if (depth == MMWLAN_RELAY_DEPTH_UNKNOWN)
    {
        snprintf(buf, len, "unknown");
    }
    else
    {
        snprintf(buf, len, "%u", (unsigned)depth);
    }
}

/* "relay", "root" or "other": which AP the STA is associated with. */
static const char *classify_bssid(const uint8_t bssid[NODE_MAC_LEN])
{
    uint8_t ref[NODE_MAC_LEN];

    if (node_parse_mac(LAB_RELAY_AP_BSSID_STR, ref) && node_mac_equal(bssid, ref))
    {
        return "relay";
    }
    if (node_parse_mac(LAB_ROOT_AP_BSSID_STR, ref) && node_mac_equal(bssid, ref))
    {
        return "root";
    }
    return "other";
}

static void halt(const char *why)
{
    lab_outf("EVENT,node,t_ms=%" PRIu32 ",ev=fatal,reason=%s", node_uptime_ms(), why);
    while (true)
    {
        ESP_LOGE(TAG, "Halted: %s", why);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void build_sta_args(void)
{
    memcpy(sta_args.ssid, LAB_SSID, strlen(LAB_SSID));
    sta_args.ssid_len = strlen(LAB_SSID);
    sta_args.security_type = MMWLAN_SAE;
    memcpy(sta_args.passphrase, LAB_PASSPHRASE, strlen(LAB_PASSPHRASE));
    /* Explicit length: with 0, morselib uses strlen(ssid) instead (issue #11). */
    sta_args.passphrase_len = strlen(LAB_PASSPHRASE);
    sta_args.pmf_mode = LAB_PMF_REQUIRED ? MMWLAN_PMF_REQUIRED : MMWLAN_PMF_DISABLED;
    sta_args.scan_interval_base_s = CONFIG_LAB_A_SCAN_INTERVAL_BASE_S;
    sta_args.scan_interval_limit_s = CONFIG_LAB_A_SCAN_INTERVAL_LIMIT_S;

#if CONFIG_LAB_A_PIN_RELAY
    if (!node_parse_mac(LAB_RELAY_AP_BSSID_STR, sta_args.bssid))
    {
        halt("bad_relay_bssid");
    }
#elif CONFIG_LAB_A_PIN_ROOT
    if (!node_parse_mac(LAB_ROOT_AP_BSSID_STR, sta_args.bssid))
    {
        ESP_LOGE(TAG, "LAB_A_PIN_ROOT needs CONFIG_LAB_ROOT_AP_BSSID (node B's BSSID)");
        halt("bad_root_bssid");
    }
#endif
}

/* Returns true once the STA (or relay STA) has been enabled. */
static bool wlan_start(void)
{
    esp_err_t err;

#if CONFIG_A_MODE_RELAY_STA
    struct mmwlan_relay_args relay_args = MMWLAN_RELAY_ARGS_INIT;

    relay_args.relay_sta_args = &sta_args;
    relay_args.sta_status_cb = sta_status_cb;
    err = mmhalow_relay_enable(&relay_args);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mmhalow_relay_enable failed: %s", esp_err_to_name(err));
    }
#else
    mmhalow_wifi_config_t conf;

    memset(&conf, 0, sizeof(conf));
    conf.sta = sta_args;
    err = mmhalow_set_config(WIFI_IF_STA, &conf);
    if (err == ESP_OK)
    {
        /* mmhalow_connect() returns the mmwlan_status of mmwlan_sta_enable(); 0 is success. */
        err = mmhalow_connect(sta_status_cb);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Unable to enable the STA: %d", (int)err);
    }
#endif

    lab_outf("EVENT,wlan,t_ms=%" PRIu32 ",ev=%s,mode=%s,ssid=%s,pin=%s,err=%d",
             node_uptime_ms(),
             (err == ESP_OK) ? "sta_start" : "sta_start_failed",
             MODE_NAME,
             LAB_SSID,
             PIN_NAME,
             (int)err);
    return err == ESP_OK;
}

static void handle_event(const struct node_event *evt)
{
    char depth[8];

    switch (evt->type)
    {
        case EVENT_STA_STATE:
        {
            char bssid_str[NODE_MAC_STR_LEN] = "none";
            uint8_t bssid[NODE_MAC_LEN];

            if (evt->sta_state == MMWLAN_STA_CONNECTED && mmwlan_get_bssid(bssid) == MMWLAN_SUCCESS)
            {
                node_format_mac(bssid, bssid_str);
            }
            lab_outf("EVENT,wlan,t_ms=%" PRIu32 ",ev=sta_state,state=%s,bssid=%s",
                     node_uptime_ms(),
                     sta_state_str(evt->sta_state),
                     bssid_str);
            break;
        }

        case EVENT_RELAY_STATUS:
            format_depth(evt->relay.depth, depth, sizeof(depth));
            lab_outf("EVENT,relay,t_ms=%" PRIu32 ",ev=status,state=%s,sta_state=%s,depth=%s",
                     node_uptime_ms(),
                     relay_state_str(evt->relay.state),
                     sta_state_str(evt->relay.sta_state),
                     depth);
            break;
    }
}

static void install_static_arp(void)
{
    node_static_arp_set("node_b", LAB_IP_NODE_B, STATIC_ARP_NODE_B_MAC, false);
    node_static_arp_set("relay", LAB_IP_RELAY, STATIC_ARP_RELAY_MAC, false);
}

static void log_link_up(uint32_t down_ms)
{
    char bssid_str[NODE_MAC_STR_LEN] = "unknown";
    const char *peer = "unknown";
    uint8_t bssid[NODE_MAC_LEN];
    char depth[8] = "na";

    if (mmwlan_get_bssid(bssid) == MMWLAN_SUCCESS)
    {
        node_format_mac(bssid, bssid_str);
        peer = classify_bssid(bssid);
    }

    /* Without CONFIG_HALOW_AP_MODE esp-halow does not compile umac_ap.c, and morselib's
     * mmwlan_get_vif_channel_info() references umac_ap_get_channel_info() for MMWLAN_VIF_AP, so
     * calling it would fail to link. Plain-STA builds report chan=0/op_class=0 instead. */
    struct mmwlan_vif_channel_info chan = { 0 };
#if CONFIG_HALOW_AP_MODE
    (void)mmwlan_get_vif_channel_info(MMWLAN_VIF_STA, &chan);
#endif

#if CONFIG_A_MODE_RELAY_STA
    struct mmwlan_relay_status relay_status;
    if (mmwlan_relay_get_status(&relay_status) == MMWLAN_SUCCESS)
    {
        format_depth(relay_status.depth, depth, sizeof(depth));
    }
#endif

    lab_outf("EVENT,wlan,t_ms=%" PRIu32 ",ev=link_up,bssid=%s,peer=%s,via_relay=%d,pin=%s,"
             "rssi=%" PRId32 ",chan=%u,op_class=%u,depth=%s,down_ms=%" PRIu32,
             node_uptime_ms(),
             bssid_str,
             peer,
             (strcmp(peer, "relay") == 0) ? 1 : 0,
             PIN_NAME,
             mmwlan_get_rssi(),
             (unsigned)chan.s1g_chan_num,
             (unsigned)chan.op_class,
             depth,
             down_ms);

    if (chan.s1g_chan_num != 0 && chan.s1g_chan_num != LAB_S1G_CHAN_NUM)
    {
        ESP_LOGW(TAG,
                 "Joined S1G channel %u, the lab plan uses %d",
                 (unsigned)chan.s1g_chan_num,
                 LAB_S1G_CHAN_NUM);
    }
#if CONFIG_LAB_A_PIN_RELAY
    if (strcmp(peer, "relay") != 0)
    {
        ESP_LOGW(TAG, "Pinned to the relay but associated with %s", bssid_str);
    }
#endif
}

/* ---- Main --------------------------------------------------------------------------------- */

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
    svc_done = xEventGroupCreate();
    assert(event_queue != NULL && svc_done != NULL);
    xEventGroupSetBits(svc_done, SVC_ALL_BITS);

    if (strcmp(LAB_COUNTRY, CONFIG_HALOW_COUNTRY_CODE) != 0)
    {
        ESP_LOGE(TAG,
                 "CONFIG_LAB_COUNTRY \"%s\" differs from CONFIG_HALOW_COUNTRY_CODE \"%s\"",
                 LAB_COUNTRY,
                 CONFIG_HALOW_COUNTRY_CODE);
        halt("country_mismatch");
    }

    ESP_ERROR_CHECK(mmhalow_init(NULL));
    halow_netif = mmhalow_get_netif();
    assert(halow_netif != NULL);

    esp_err_t err = node_set_static_ip(halow_netif, LAB_IP_NODE_A, LAB_NETMASK, LAB_GATEWAY);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Static IP setup failed: %s", esp_err_to_name(err));
        halt("static_ip_failed");
    }

    uint8_t mac[NODE_MAC_LEN] = { 0 };
    char mac_str[NODE_MAC_STR_LEN];
    (void)esp_netif_get_mac(halow_netif, mac);
    node_format_mac(mac, mac_str);
    lab_outf("EVENT,node,t_ms=%" PRIu32 ",ev=boot,app=node_a,role=%s,mac=%s,ip=%s,netmask=%s,"
             "country=%s,pin=%s,bssid_pin=%s",
             node_uptime_ms(),
             MODE_NAME,
             mac_str,
             LAB_IP_NODE_A,
             LAB_NETMASK,
             LAB_COUNTRY,
             PIN_NAME,
             PIN_BSSID);

    build_sta_args();

#if CONFIG_LAB_A_TPUT_SERVER
    /* The sinks bind INADDR_ANY and run for the life of the node, across link changes. */
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

#if CONFIG_A_MODE_RELAY_STA
    if (mmwlan_register_relay_status_cb(relay_status_cb, NULL) != MMWLAN_SUCCESS)
    {
        ESP_LOGW(TAG, "Unable to register the relay status callback");
    }
#endif

    bool wlan_started = wlan_start();
    uint32_t next_wlan_retry_ms = node_uptime_ms() + WLAN_START_RETRY_MS;

    bool link_up = false;
    bool start_pending = false;
    uint32_t start_at_ms = 0;
    uint32_t link_change_ms = node_uptime_ms();
    uint32_t next_health_ms = node_uptime_ms() + CONFIG_LAB_A_HEALTH_INTERVAL_MS;

    while (true)
    {
        struct node_event evt;
        if (xQueueReceive(event_queue, &evt, pdMS_TO_TICKS(SUPERVISOR_POLL_MS)) == pdTRUE)
        {
            handle_event(&evt);
        }

        uint32_t now = node_uptime_ms();

        if (!wlan_started && node_time_reached(now, next_wlan_retry_ms))
        {
            wlan_started = wlan_start();
            next_wlan_retry_ms = now + WLAN_START_RETRY_MS;
        }

        bool up = esp_netif_is_netif_up(halow_netif);
        if (up && !link_up)
        {
            log_link_up(now - link_change_ms);
            link_up = true;
            link_change_ms = now;
            install_static_arp();
            if (svc_state != SVC_RUNNING)
            {
                start_pending = true;
                start_at_ms = now + CONFIG_LAB_A_SERVICE_START_DELAY_MS;
            }
        }
        else if (!up && link_up)
        {
            lab_outf("EVENT,wlan,t_ms=%" PRIu32 ",ev=link_down,up_ms=%" PRIu32,
                     now,
                     now - link_change_ms);
            link_up = false;
            link_change_ms = now;
            start_pending = false;
            if (STOP_SERVICES_ON_LINK_DOWN)
            {
                services_stop("link_down");
            }
        }

        supervisor_poll_services();
        if (start_pending &&
            link_up &&
            svc_state == SVC_IDLE &&
            node_time_reached(node_uptime_ms(), start_at_ms))
        {
            start_pending = false;
            services_start();
        }

        if (node_time_reached(now, next_health_ms))
        {
            static const char *const svc_state_names[] = { "idle", "running", "stopping" };
            char extra[128];

            next_health_ms = now + CONFIG_LAB_A_HEALTH_INTERVAL_MS;
            snprintf(extra,
                     sizeof(extra),
                     ",role=%s,link=%d,rssi=%" PRId32 ",svc=%s,evt_drops=%" PRIu32,
                     MODE_NAME,
                     link_up ? 1 : 0,
                     link_up ? mmwlan_get_rssi() : INT32_MIN,
                     svc_state_names[svc_state],
                     event_queue_drops);
            node_log_health(extra);
        }
    }
}
