/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * MM6108-EKH05 S1G relay repeater for the A -> relay -> B HaLow lab.
 *
 * Boot sequence: load config (defaults + config store) -> mmwlan init with the country's channel
 * list -> boot the MM6108 -> mmipal (static IPv4 on the STA VIF) -> mmwlan_relay_enable() as a
 * repeater (relay STA towards the root, relay AP for node A) -> UDP echo service -> monitor loop.
 *
 * MMWLAN/mmipal callbacks only copy their arguments into a queue; the monitor task prints the
 * EVENT lines, calls MMWLAN getters and drives the LED, so no callback blocks or calls MMWLAN.
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"

#include "mmhal_app.h"
#include "mmhal_os.h"
#include "mmipal.h"
#include "mmosal.h"
#include "mmregdb.h"
#include "mmutils.h"
#include "mmwlan.h"

#include "halow_lab_config.h"
#include "lab_platform.h"
#include "lab_services.h"
#include "relay_config.h"
#include "relay_events.h"
#include "relay_led.h"

#define RELAY_EVQ_DEPTH 24
#define RELAY_UDP_TASK_STACK 6144
#define RELAY_SVC_RETRY_MS 1000
#define RELAY_FATAL_REBOOT_DELAY_MS 1000

enum relay_evt_type
{
    RELAY_EVT_RELAY_STATUS,
    RELAY_EVT_STA_STATE,
    RELAY_EVT_AP_STA,
    RELAY_EVT_LINK,
    RELAY_EVT_FATAL,
};

struct relay_evt
{
    enum relay_evt_type type;
    union
    {
        struct mmwlan_relay_status relay;
        enum mmwlan_sta_state sta_state;
        struct mmwlan_ap_sta_status ap_sta;
        struct mmipal_link_status link;
        struct
        {
            uint32_t fileid;
            uint32_t line;
        } fatal;
    } u;
};

struct relay_ap_sta_entry
{
    bool in_use;
    bool authorized;
    uint16_t aid;
    uint8_t mac[MMWLAN_MAC_ADDR_LEN];
};

/** Monitor-task state (only touched by the monitor task, except the drop counter). */
struct relay_state
{
    struct mmwlan_relay_status relay;
    uint8_t parent_bssid[MMWLAN_MAC_ADDR_LEN];
    bool link_up;
    /** STA connection as last reported by the STA status callback (for drop counting). */
    bool sta_connected;
    struct relay_ap_sta_entry ap_stas[MMWLAN_AP_MAX_STAS_LIMIT];
    uint8_t num_authorized;
    uint32_t sta_connects;
    uint32_t sta_drops;
    uint32_t ap_joins;
    uint32_t ap_leaves;
    volatile uint32_t evt_dropped;
};

static struct relay_cfg cfg;
static struct relay_state state = {
    .relay = {
        .state = MMWLAN_RELAY_STATE_DISABLED,
        .sta_state = MMWLAN_STA_DISABLED,
        .depth = MMWLAN_RELAY_DEPTH_UNKNOWN,
    },
};
static struct mmosal_queue *evq;
static struct mmwlan_fatal_error_args fatal_args;

/* ---- Callbacks (MMWLAN / lwIP context): copy and enqueue only ------------------------------ */

static void evq_push(const struct relay_evt *evt)
{
    if (evq == NULL || !mmosal_queue_push(evq, evt, 0))
    {
        MMOSAL_TASK_ENTER_CRITICAL();
        state.evt_dropped++;
        MMOSAL_TASK_EXIT_CRITICAL();
    }
}

static void relay_status_cb(const struct mmwlan_relay_status *status, void *arg)
{
    MM_UNUSED(arg);
    struct relay_evt evt = { .type = RELAY_EVT_RELAY_STATUS };
    evt.u.relay = *status;
    evq_push(&evt);
}

static void sta_status_cb(enum mmwlan_sta_state sta_state)
{
    struct relay_evt evt = { .type = RELAY_EVT_STA_STATE };
    evt.u.sta_state = sta_state;
    evq_push(&evt);
}

static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *sta_status, void *arg)
{
    MM_UNUSED(arg);
    struct relay_evt evt = { .type = RELAY_EVT_AP_STA };
    evt.u.ap_sta = *sta_status;
    evq_push(&evt);
}

static void link_status_cb(const struct mmipal_link_status *link_status)
{
    struct relay_evt evt = { .type = RELAY_EVT_LINK };
    evt.u.link = *link_status;
    evq_push(&evt);
}

static void fatal_error_cb(struct mmwlan_fatal_error_args *args)
{
    struct relay_evt evt = { .type = RELAY_EVT_FATAL };
    evt.u.fatal.fileid = args->fileid;
    evt.u.fatal.line = args->line;
    evq_push(&evt);
}

/* ---- Helpers ------------------------------------------------------------------------------- */

static const char *relay_state_str(enum mmwlan_relay_state s)
{
    switch (s)
    {
        case MMWLAN_RELAY_STATE_DISABLED:
            return "disabled";

        case MMWLAN_RELAY_STATE_ROOT:
            return "root";

        case MMWLAN_RELAY_STATE_STA:
            return "sta";

        case MMWLAN_RELAY_STATE_REPEATER:
            return "repeater";
    }
    return "unknown";
}

static const char *sta_state_str(enum mmwlan_sta_state s)
{
    switch (s)
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

static const char *ap_sta_state_str(enum mmwlan_ap_sta_state s)
{
    switch (s)
    {
        case MMWLAN_AP_STA_UNKNOWN:
            return "unknown";

        case MMWLAN_AP_STA_AUTHENTICATED:
            return "authenticated";

        case MMWLAN_AP_STA_ASSOCIATED:
            return "associated";

        case MMWLAN_AP_STA_AUTHORIZED:
            return "authorized";
    }
    return "invalid";
}

/* "N" or "unknown" for MMWLAN_RELAY_DEPTH_UNKNOWN. */
static const char *depth_str(uint8_t depth, char *buf, size_t len)
{
    if (depth == MMWLAN_RELAY_DEPTH_UNKNOWN)
    {
        return "unknown";
    }
    snprintf(buf, len, "%u", depth);
    return buf;
}

static void led_refresh(void)
{
    relay_led_update(state.relay.sta_state == MMWLAN_STA_CONNECTED, state.relay.depth,
                     state.num_authorized);
}

/* Fatal setup error: report, show red and keep reporting (the device needs attention). */
static void halt_with_error(const char *what, int code)
{
    relay_led_set_failed();
    while (true)
    {
        relay_event("error", "what=%s,code=%d", what, code);
        mmosal_task_sleep(10000);
    }
}

static void install_static_arp(void)
{
    for (unsigned ii = 0; ii < cfg.num_static_arp; ii++)
    {
        struct mmipal_arp_config arp;

        memset(&arp, 0, sizeof(arp));
        mmosal_safer_strcpy(arp.ip_addr, cfg.static_arp[ii].ip, sizeof(arp.ip_addr));
        memcpy(arp.mac_addr, cfg.static_arp[ii].mac, sizeof(arp.mac_addr));
        enum mmipal_status st = mmipal_add_static_arp_entry(&arp);
        relay_event("static_arp", "ip=%s,mac=" MM_MAC_ADDR_FMT ",status=%d", arp.ip_addr,
                    MM_MAC_ADDR_VAL(arp.mac_addr), (int)st);
    }
}

/* ---- Event handlers (monitor task) --------------------------------------------------------- */

/*
 * The relay AP is torn down when the path to the root is lost for longer than the recovery
 * timeout; forget its stations then so ap_stas and the LED do not show stale entries.
 */
static void ap_sta_flush(const char *reason)
{
    for (unsigned ii = 0; ii < MM_ARRAY_COUNT(state.ap_stas); ii++)
    {
        struct relay_ap_sta_entry *e = &state.ap_stas[ii];
        if (!e->in_use)
        {
            continue;
        }
        if (e->authorized)
        {
            state.num_authorized--;
            state.ap_leaves++;
            relay_event("ap_sta_leave", "mac=" MM_MAC_ADDR_FMT ",aid=%u,state=%s,ap_stas=%u",
                        MM_MAC_ADDR_VAL(e->mac), e->aid, reason, state.num_authorized);
        }
        e->in_use = false;
    }
}

static void handle_relay_status(const struct mmwlan_relay_status *st)
{
    char dbuf[4];

    if (st->state != MMWLAN_RELAY_STATE_REPEATER)
    {
        ap_sta_flush("ap_down");
    }
    state.relay = *st;
    memset(state.parent_bssid, 0, sizeof(state.parent_bssid));
    if (st->sta_state == MMWLAN_STA_CONNECTED)
    {
        (void)mmwlan_get_bssid(state.parent_bssid);
    }

    relay_event("relay_status", "state=%s,sta_state=%s,depth=%s,parent=" MM_MAC_ADDR_FMT,
                relay_state_str(st->state), sta_state_str(st->sta_state),
                depth_str(st->depth, dbuf, sizeof(dbuf)), MM_MAC_ADDR_VAL(state.parent_bssid));
    led_refresh();
}

static void handle_sta_state(enum mmwlan_sta_state sta_state)
{
    bool connected = sta_state == MMWLAN_STA_CONNECTED;

    if (connected && !state.sta_connected)
    {
        state.sta_connects++;
    }
    else if (!connected && state.sta_connected)
    {
        state.sta_drops++;
    }
    state.sta_connected = connected;
    state.relay.sta_state = sta_state;

    uint8_t bssid[MMWLAN_MAC_ADDR_LEN] = { 0 };
    int32_t rssi = INT32_MIN;
    if (sta_state == MMWLAN_STA_CONNECTED)
    {
        (void)mmwlan_get_bssid(bssid);
        rssi = mmwlan_get_rssi();
    }

    if (rssi != INT32_MIN)
    {
        relay_event("sta_state", "state=%s,bssid=" MM_MAC_ADDR_FMT ",rssi_dbm=%ld",
                    sta_state_str(sta_state), MM_MAC_ADDR_VAL(bssid), (long)rssi);
    }
    else
    {
        relay_event("sta_state", "state=%s,bssid=" MM_MAC_ADDR_FMT, sta_state_str(sta_state),
                    MM_MAC_ADDR_VAL(bssid));
    }
    led_refresh();
}

static struct relay_ap_sta_entry *ap_sta_find(const uint8_t *mac, bool alloc)
{
    struct relay_ap_sta_entry *free_slot = NULL;

    for (unsigned ii = 0; ii < MM_ARRAY_COUNT(state.ap_stas); ii++)
    {
        struct relay_ap_sta_entry *e = &state.ap_stas[ii];
        if (e->in_use && memcmp(e->mac, mac, MMWLAN_MAC_ADDR_LEN) == 0)
        {
            return e;
        }
        if (!e->in_use && free_slot == NULL)
        {
            free_slot = e;
        }
    }
    if (alloc && free_slot != NULL)
    {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->in_use = true;
        memcpy(free_slot->mac, mac, MMWLAN_MAC_ADDR_LEN);
        return free_slot;
    }
    return NULL;
}

static void handle_ap_sta(const struct mmwlan_ap_sta_status *st)
{
    bool gone = st->state == MMWLAN_AP_STA_UNKNOWN;
    struct relay_ap_sta_entry *e = ap_sta_find(st->mac_addr, !gone);
    bool was_authorized = e != NULL && e->authorized;
    bool now_authorized = st->state == MMWLAN_AP_STA_AUTHORIZED;

    if (e != NULL)
    {
        e->aid = st->aid;
        e->authorized = now_authorized;
        if (gone)
        {
            e->in_use = false;
        }
    }
    if (now_authorized && !was_authorized)
    {
        state.num_authorized++;
        state.ap_joins++;
    }
    else if (!now_authorized && was_authorized)
    {
        state.num_authorized--;
        state.ap_leaves++;
    }

    relay_event("ap_sta", "mac=" MM_MAC_ADDR_FMT ",aid=%u,state=%s,ap_stas=%u",
                MM_MAC_ADDR_VAL(st->mac_addr), st->aid, ap_sta_state_str(st->state),
                state.num_authorized);
    if (now_authorized && !was_authorized)
    {
        relay_event("ap_sta_join", "mac=" MM_MAC_ADDR_FMT ",aid=%u,ap_stas=%u",
                    MM_MAC_ADDR_VAL(st->mac_addr), st->aid, state.num_authorized);
    }
    else if (!now_authorized && was_authorized)
    {
        relay_event("ap_sta_leave", "mac=" MM_MAC_ADDR_FMT ",aid=%u,state=%s,ap_stas=%u",
                    MM_MAC_ADDR_VAL(st->mac_addr), st->aid, ap_sta_state_str(st->state),
                    state.num_authorized);
    }
    led_refresh();
}

static void handle_link(const struct mmipal_link_status *link)
{
    state.link_up = link->link_state == MMIPAL_LINK_UP;
    relay_event("link", "state=%s,ip=%s,netmask=%s,gateway=%s", state.link_up ? "up" : "down",
                link->ip_addr, link->netmask, link->gateway);
    if (state.link_up)
    {
        install_static_arp();
    }
}

static void handle_fatal(uint32_t fileid, uint32_t line)
{
    relay_event("fatal_error", "fileid=0x%08lx,line=%lu,action=reboot", (unsigned long)fileid,
                (unsigned long)line);
    relay_led_set_failed();
    mmosal_task_sleep(RELAY_FATAL_REBOOT_DELAY_MS);
    mmhal_reset();
}

static void report_health(void)
{
    char dbuf[4];
    char rssi[12] = "na";
    uint32_t tx_pkts = 0;
    uint32_t rx_pkts = 0;

    if (state.relay.sta_state == MMWLAN_STA_CONNECTED)
    {
        int32_t v = mmwlan_get_rssi();
        if (v != INT32_MIN)
        {
            snprintf(rssi, sizeof(rssi), "%ld", (long)v);
        }
    }
    mmipal_get_link_packet_counts(&tx_pkts, &rx_pkts);

    relay_event("health",
                "uptime_s=%lu,state=%s,sta_state=%s,depth=%s,parent=" MM_MAC_ADDR_FMT
                ",rssi_dbm=%s,ap_stas=%u,link=%s,ip_tx_pkts=%lu,ip_rx_pkts=%lu,heap_free=%lu,"
                "heap_min=%lu,sta_connects=%lu,sta_drops=%lu,ap_joins=%lu,ap_leaves=%lu,"
                "ev_dropped=%lu",
                (unsigned long)(mmosal_get_time_ms() / 1000), relay_state_str(state.relay.state),
                sta_state_str(state.relay.sta_state),
                depth_str(state.relay.depth, dbuf, sizeof(dbuf)),
                MM_MAC_ADDR_VAL(state.parent_bssid), rssi, state.num_authorized,
                state.link_up ? "up" : "down", (unsigned long)tx_pkts, (unsigned long)rx_pkts,
                (unsigned long)xPortGetFreeHeapSize(),
                (unsigned long)xPortGetMinimumEverFreeHeapSize(),
                (unsigned long)state.sta_connects, (unsigned long)state.sta_drops,
                (unsigned long)state.ap_joins, (unsigned long)state.ap_leaves,
                (unsigned long)state.evt_dropped);
}

/* ---- Startup ------------------------------------------------------------------------------- */

static void select_channel_list(void)
{
    const struct mmwlan_s1g_channel_list *list =
        mmwlan_lookup_regulatory_domain(get_regulatory_db(), cfg.country);
    if (list == NULL)
    {
        relay_event("regdb", "country=%s,found=0", cfg.country);
        halt_with_error("unknown_country", 0);
    }

    enum mmwlan_status st = mmwlan_set_channel_list(list);
    if (st != MMWLAN_SUCCESS)
    {
        halt_with_error("set_channel_list", st);
    }

    /* The relay follows the root's channel; report whether the lab channel is in the list. */
    const struct mmwlan_s1g_channel *lab_chan = NULL;
    for (unsigned ii = 0; ii < list->num_channels; ii++)
    {
        const struct mmwlan_s1g_channel *c = &list->channels[ii];
        if (c->s1g_chan_num == LAB_S1G_CHAN_NUM && c->global_operating_class == LAB_OP_CLASS)
        {
            lab_chan = c;
            break;
        }
    }
    if (lab_chan != NULL)
    {
        relay_event("regdb",
                    "country=%s,found=1,num_channels=%u,lab_chan=%u,lab_op_class=%d,"
                    "lab_s1g_op_class=%d,lab_freq_hz=%lu,lab_bw_mhz=%u",
                    (const char *)list->country_code, list->num_channels, LAB_S1G_CHAN_NUM,
                    lab_chan->global_operating_class, lab_chan->s1g_operating_class,
                    (unsigned long)lab_chan->centre_freq_hz, lab_chan->bw_mhz);
    }
    else
    {
        relay_event("regdb", "country=%s,found=1,num_channels=%u,lab_chan=%u,lab_chan_found=0",
                    (const char *)list->country_code, list->num_channels, LAB_S1G_CHAN_NUM);
    }
}

/*
 * BCF name for the boot line. The EKH05 BCF describes itself as JSON ({"module": "...", ...}),
 * which would break the comma-separated key=value format: use the module name if there is one,
 * otherwise the description with separators replaced.
 */
static void bcf_name(const char *desc, char *out, size_t out_len)
{
    const char *start = desc;
    size_t len = strlen(desc);
    const char *key = strstr(desc, "\"module\"");

    if (key != NULL)
    {
        const char *colon = strchr(key + 8, ':');
        const char *open = (colon != NULL) ? strchr(colon, '"') : NULL;
        const char *close = (open != NULL) ? strchr(open + 1, '"') : NULL;
        if (close != NULL)
        {
            start = open + 1;
            len = (size_t)(close - start);
        }
    }

    if (len >= out_len)
    {
        len = out_len - 1;
    }
    for (size_t ii = 0; ii < len; ii++)
    {
        char c = start[ii];
        out[ii] = (c == ',' || c == '=' || c == ' ' || c == '"' || c == '{' || c == '}') ? '_' : c;
    }
    out[len] = '\0';
    if (len == 0 && out_len > 1)
    {
        out[0] = '?';
        out[1] = '\0';
    }
}

static void boot_chip(void)
{
    struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
    enum mmwlan_status st = mmwlan_boot(&boot_args);
    if (st != MMWLAN_SUCCESS)
    {
        halt_with_error("mmwlan_boot", st);
    }

    struct mmwlan_version version;
    struct mmwlan_bcf_metadata bcf;
    uint8_t sta_mac[MMWLAN_MAC_ADDR_LEN] = { 0 };
    char hw[32] = "unknown";
    char bcf_str[48];

    memset(&version, 0, sizeof(version));
    memset(&bcf, 0, sizeof(bcf));
    (void)mmwlan_get_version(&version);
    (void)mmwlan_get_bcf_metadata(&bcf);
    (void)mmwlan_get_vif_mac_addr(MMWLAN_VIF_STA, sta_mac);
    (void)mmhal_get_hardware_version(hw, sizeof(hw));
    bcf_name(bcf.board_desc, bcf_str, sizeof(bcf_str));

    /* sta_mac is the relay's IP/ARP address (mmipal binds to the STA VIF), not the AP BSSID. */
    relay_event("boot",
                "hw=%s,morselib=%s,fw=%s,chip_id=0x%04lx,chip=%s,bcf=%s,bcf_api=%u.%u.%u,"
                "sta_mac=" MM_MAC_ADDR_FMT,
                hw, version.morselib_version, version.morse_fw_version,
                (unsigned long)version.morse_chip_id, version.morse_chip_id_string,
                bcf_str, bcf.version.major,
                bcf.version.minor, bcf.version.patch, MM_MAC_ADDR_VAL(sta_mac));
}

static void apply_scan_config(void)
{
    if (cfg.num_scan_channels == 0)
    {
        return;
    }

    struct mmwlan_scan_config scan = MMWLAN_SCAN_CONFIG_INIT;
    scan.selected_channels = cfg.scan_channels;
    scan.selected_channels_len = cfg.num_scan_channels;
    scan.selective_scan_attempts = cfg.scan_attempts;
    enum mmwlan_status st = mmwlan_set_scan_config(&scan);
    if (st != MMWLAN_SUCCESS)
    {
        relay_event("warning", "what=set_scan_config,code=%d", st);
    }
}

static void start_relay(void)
{
    static struct mmwlan_relay_args relay_args = MMWLAN_RELAY_ARGS_INIT;
    enum mmwlan_status st;

    if (cfg.depth_override != 0)
    {
        st = mmwlan_relay_ate_set_depth_override(cfg.depth_override);
        if (st != MMWLAN_SUCCESS)
        {
            relay_event("warning", "what=relay_depth_override,code=%d", st);
        }
    }

    st = mmwlan_register_relay_status_cb(relay_status_cb, NULL);
    if (st != MMWLAN_SUCCESS)
    {
        halt_with_error("register_relay_status_cb", st);
    }

    cfg.ap_args.sta_status_cb = ap_sta_status_cb;
    cfg.ap_args.sta_status_cb_arg = NULL;

    relay_args.forwarding_table_size = cfg.forwarding_table_size;
    relay_args.recovery_timeout_ms = cfg.recovery_timeout_ms;
    relay_args.relay_sta_args = &cfg.sta_args;
    relay_args.sta_status_cb = sta_status_cb;
    relay_args.relay_ap_args = &cfg.ap_args;
    relay_args.root_ap_args = NULL;

    st = mmwlan_relay_enable(&relay_args);
    relay_event("relay_enable", "role=repeater,status=%d", st);
    if (st != MMWLAN_SUCCESS)
    {
        halt_with_error("mmwlan_relay_enable", st);
    }
}

static void udp_echo_task(void *arg)
{
    MM_UNUSED(arg);
    struct udp_echo_server_cfg scfg;

    udp_echo_server_cfg_init(&scfg);
    scfg.port = cfg.udp_echo_port;
    scfg.report_interval_ms = cfg.report_interval_ms;

    while (true)
    {
        int rc = udp_echo_server_run(&scfg);
        relay_event("warning", "what=udp_echo_server,code=%d,retry_ms=%u", rc,
                    RELAY_SVC_RETRY_MS);
        mmosal_task_sleep(RELAY_SVC_RETRY_MS);
    }
}

static void monitor_loop(void)
{
    uint32_t next_health = mmosal_get_time_ms() + cfg.health_interval_ms;

    report_health();
    while (true)
    {
        struct relay_evt evt;
        uint32_t now = mmosal_get_time_ms();
        uint32_t wait = (int32_t)(next_health - now) > 0 ? next_health - now : 0;

        if (mmosal_queue_pop(evq, &evt, wait))
        {
            switch (evt.type)
            {
                case RELAY_EVT_RELAY_STATUS:
                    handle_relay_status(&evt.u.relay);
                    break;

                case RELAY_EVT_STA_STATE:
                    handle_sta_state(evt.u.sta_state);
                    break;

                case RELAY_EVT_AP_STA:
                    handle_ap_sta(&evt.u.ap_sta);
                    break;

                case RELAY_EVT_LINK:
                    handle_link(&evt.u.link);
                    break;

                case RELAY_EVT_FATAL:
                    handle_fatal(evt.u.fatal.fileid, evt.u.fatal.line);
                    break;
            }
        }

        now = mmosal_get_time_ms();
        if ((int32_t)(now - next_health) >= 0)
        {
            report_health();
            next_health += cfg.health_interval_ms;
            if ((int32_t)(now - next_health) >= 0)
            {
                next_health = now + cfg.health_interval_ms;
            }
        }
    }
}

void app_init(void)
{
    lab_platform_init();
    printf("\n\nHaLow lab relay (EKH05 repeater), built " __DATE__ " " __TIME__ "\n\n");

    evq = mmosal_queue_create(RELAY_EVQ_DEPTH, sizeof(struct relay_evt), "relay_evq");
    MMOSAL_ASSERT(evq != NULL);

    relay_cfg_load(&cfg);
    relay_led_init(cfg.led_enabled);
    led_refresh();

    mmwlan_init();
    mmwlan_register_fatal_error_handler(fatal_error_cb, &fatal_args);
    select_channel_list();
    boot_chip();
    relay_cfg_print(&cfg);

    if (cfg.ip_args.mode == MMIPAL_DHCP)
    {
        lab_log(LAB_LOG_WRN, "DHCP selected; the lab plan uses static addressing");
    }
    /* mmipal_init() clears the link callback, so register it afterwards (as the SDK does). The
     * link only comes up once the relay STA associates, which happens after this point. */
    enum mmipal_status ipst = mmipal_init(&cfg.ip_args);
    if (ipst != MMIPAL_SUCCESS)
    {
        halt_with_error("mmipal_init", ipst);
    }
    mmipal_set_link_status_callback(link_status_cb);

    apply_scan_config();
    start_relay();

    if (cfg.udp_echo_port != 0 &&
        lab_task_create(udp_echo_task, NULL, "udp_echo", RELAY_UDP_TASK_STACK,
                        LAB_TASK_PRIO_NORMAL) != 0)
    {
        relay_event("warning", "what=udp_echo_task_create");
    }

    monitor_loop();
}
