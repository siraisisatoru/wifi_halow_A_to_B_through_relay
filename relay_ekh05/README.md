<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# relay_ekh05: MM6108-EKH05 S1G relay (repeater)

Firmware for the middle node of the lab:

```
node A (XIAO ESP32-S3, STA)  ->  relay (MM6108-EKH05, this firmware)  ->  node B (XIAO ESP32-S3, root AP)
192.168.50.10                    192.168.50.2                              192.168.50.1
pins BSSID 02:48:4C:00:00:02     relay AP BSSID 02:48:4C:00:00:02           SSID halow-relay-lab
```

It is a native mm-iot-sdk 2.14.3 application. It is built out of tree with the SDK's make flow
and bundled STM32U5 BSP (the SDK vendors the ST HAL/LL sources). No STM32CubeIDE, no CubeMX and
no Cube-generated project are involved. Flashing is described in [flash.md](flash.md).

## What it does

1. Loads its settings: the lab defaults from `common/include/halow_lab_config.h`, overridden by
   any config store keys (see [config.hjson](config.hjson)).
2. `mmwlan_init()`. It sets the channel list for the country code (default `AU`, which mmregdb
   maps to the 802.11 REVmf list `s1g_channel_list_au_80211_revmf`, 26 channels), reports whether
   the lab channel (S1G 37, global op class 51, 920.5 MHz, 2 MHz) is in that list, then boots
   the MM6108. The BCF `bcf_mf08651_us.mbin` is linked into the image.
3. `mmipal_init()` with a static IPv4 address (default 192.168.50.2/24, no gateway).
4. `mmwlan_relay_enable()` as a **repeater**:
   - `relay_sta_args`: SSID, SAE, passphrase with an explicit `passphrase_len` (works around
     #11), PMF required, and an optional pinned root BSSID (`wlan.bssid`; the default is any).
   - `relay_ap_args`: BSSID `02:48:4C:00:00:02`, 100 TU beacon interval, DTIM 1,
     `max_stas` 4.
   - The SDK copies SSID, security, passphrase, PMF and SAE groups from the STA args into the
     relay AP, and the relay AP follows the root's channel (op_class and channel are 0). The AP
     comes up only after the STA has associated with the root.
   - The first 2 scans of each connection attempt only look at channel 37, then the full list is
     scanned (`wlan.scan_selected_channels`, `wlan.selective_scan_attempts`).
5. Runs the common **UDP echo server on port 5000**, so node A can measure the A <-> relay hop
   on its own, separately from A <-> B. ICMP echo (ping) is answered by lwIP.
6. Monitors the relay and prints machine-readable `EVENT,relay,...` lines. Every 10 s it prints
   a health line. The RGB LED shows the state.

On an MMWLAN fatal error the firmware logs `ev=fatal_error` and reboots after 1 s. If the setup
itself fails (unknown country, boot failure, relay enable error), it shows solid red and repeats
`ev=error` every 10 s.

## Build

```sh
relay_ekh05/build.sh                 # applies SDK patches, builds elf + bin + hex, prints sizes
relay_ekh05/build.sh --revert-patches  # same, then leaves third_party/mm-iot-sdk pristine
relay_ekh05/build.sh --no-patches BUILD_DIR=build_nopatch   # build the SDK as checked out
relay_ekh05/build.sh bin-with-config   # image that also contains config.hjson (flash.md)
relay_ekh05/build.sh clean
```

`build.sh` runs `make` inside the `ekh05-builder:2.14.3` Docker image, with the repository
mounted at `/work`. Before building it runs `tools/apply_sdk_patches.sh third_party/mm-iot-sdk`,
unless you pass `--no-patches`. Outputs go to `relay_ekh05/build/`, which is gitignored:
`relay_ekh05.elf`, `.bin`, `.hex`, `.map`, and on request `.mbin`, `configstore.bin` and
`relay_ekh05_with_config.bin`. Make variables can be passed through, for example
`MAX_STAS=8`, `BUILDTYPE=debug`, `BCF_MBIN_REL=` (read the BCF from the config store instead),
or `LAB_CONFIG_OVERRIDE_HEADER=my_lab.h`.

The application does not depend on the SDK patches: it builds with or without them. The
firmware does need patch 0001 to stay up, though (see [SDK bugs](#sdk-bugs-and-what-the-relay-relies-on)),
so the Makefile refuses to link while any patch is not applied to `third_party/mm-iot-sdk`.
`--no-patches` passes `ALLOW_UNPATCHED=1` to allow it; a direct `make` needs the same flag.
`make flash` only programs the existing ELF and never rebuilds it (see `flash.md`).

### Memory use (production build, mm-iot-sdk 2.14.3, GCC 14.2.1)

| Region | Used | Size | Use |
|---|---|---|---|
| FLASH (application, 0x08008000) | 931,016 B | 1,120 KiB | 81.2 % |
| RAM | 342,816 B | 768 KiB | 43.6 % |
| BOOTLOADER (reset stub, 0x08000000) | 8 B | 16 KiB | |
| CONFIG (config store, 0x08004000) | 0 B | 16 KiB | written separately |
| FILESYSTEM | 0 B | 896 KiB | unused |

`text 930,481 / data 536 / bss 342,285` (`arm-none-eabi-size`). RAM includes the static FreeRTOS
heap: 95 KiB, plus `AP_MODE_HEAP` (5,000 B per `MAX_STAS`), plus `APP_HEAP` (32 KiB). The
MM6108 firmware image (`mm6108.mbin`, 393,176 B) and the BCF are linked into flash. The unpatched build is
within a few bytes of these figures.

## Console output

Logs go to the ST-LINK virtual COM port at 115200 8N1. Machine-readable lines use the same
format as the common library (see `common/README.md`): match `^(STATS|EVENT)`, then read
comma-separated `key=value` fields and ignore unknown keys. In relay lines, `t_ms` is
milliseconds since boot. SDK log lines (`I 12345 ...`) are interleaved with them.

| `ev=` | Fields | When |
|---|---|---|
| `regdb` | `country, found, num_channels, lab_chan, lab_op_class, lab_s1g_op_class, lab_freq_hz, lab_bw_mhz` (or `lab_chan_found=0`) | channel list set |
| `boot` | `hw, morselib, fw, chip_id, chip, bcf, bcf_api, sta_mac` | MM6108 booted |
| `config` | `ssid, security, pmf, passphrase_len, country, ap_bssid, root_bssid, beacon_tu, dtim, max_stas, recovery_ms, fwd_table, depth_override, scan_chans, scan_attempts, scan_interval_s=base/limit` | after boot (no passphrase) |
| `ip_config` | `mode, ip, netmask, gateway, hostname, static_arp, udp_echo_port` | after boot |
| `relay_enable` | `role=repeater, status` | `mmwlan_relay_enable()` returned |
| `relay_status` | `state=disabled\|sta\|repeater\|root, sta_state, depth=N\|unknown, parent=<root BSSID>` | relay status callback |
| `sta_state` | `state=disabled\|connecting\|connected, bssid, rssi_dbm` (when connected) | upstream STA callback |
| `ap_sta` | `mac, aid, state=unknown\|authenticated\|associated\|authorized, ap_stas` | every relay-AP station callback |
| `ap_sta_join` | `mac, aid, ap_stas` | a station became authorized (node A joined) |
| `ap_sta_leave` | `mac, aid, state, ap_stas` (`state=ap_down` when the relay AP was torn down) | an authorized station left |
| `link` | `state=up\|down, ip, netmask, gateway` | mmipal link callback (link is the STA VIF) |
| `static_arp` | `ip, mac, status` | on link-up, for each `lab.arpN.*` entry |
| `health` | `uptime_s, state, sta_state, depth, parent, rssi_dbm (na when not connected), ap_stas, link, ip_tx_pkts, ip_rx_pkts, heap_free, heap_min, sta_connects, sta_drops, ap_joins, ap_leaves, ev_dropped` | every `lab.health_interval_ms` (10 s) |
| `warning` | `what, code, ...` | non-fatal problem (for example `set_scan_config`, or the UDP service restarting) |
| `error` | `what, code` | setup failed (repeats every 10 s) |
| `fatal_error` | `fileid, line, action=reboot` | MMWLAN fatal error |

The UDP echo service prints the common `EVENT,udp_srv,...` and `STATS,udp_srv,...` lines
(`listening`, `session`, `session_end`, and STATS every 5 s). The flow name is the same as on
node B, `udp_srv`, because each board has its own console. The relay's `lost` counts are
**uplink** loss on the A -> relay hop only.

Example (shape only):

```
EVENT,relay,t_ms=1830,ev=boot,hw=...,morselib=...,fw=...,chip_id=0x....,chip=...,bcf=...,bcf_api=...,sta_mac=xx:xx:xx:xx:xx:xx
EVENT,relay,t_ms=1834,ev=config,ssid=halow-relay-lab,security=sae,pmf=required,passphrase_len=18,country=AU,ap_bssid=02:48:4c:00:00:02,root_bssid=00:00:00:00:00:00,beacon_tu=100,dtim=1,max_stas=4,recovery_ms=10000,fwd_table=64,depth_override=0,scan_chans=37,scan_attempts=2,scan_interval_s=2/10
EVENT,relay,t_ms=7410,ev=relay_status,state=repeater,sta_state=connected,depth=1,parent=xx:xx:xx:xx:xx:xx
EVENT,relay,t_ms=9120,ev=ap_sta_join,mac=xx:xx:xx:xx:xx:xx,aid=1,ap_stas=1
EVENT,relay,t_ms=10002,ev=health,uptime_s=10,state=repeater,sta_state=connected,depth=1,parent=...,rssi_dbm=-41,ap_stas=1,link=up,...
```

## LED (EKH05 RGB LED)

| Pattern | Meaning |
|---|---|
| Blue, slow blink (1 s on, 1 s off) | Upstream STA searching for or connecting to the root |
| Blue, solid | Associated, relay depth not known yet |
| N green pulses, then M blue pulses, then a 2 s pause | Running at depth N (1 = directly under the root), with M authorized downstream STAs |
| Red, solid | Setup or fatal error |

Set `lab.led_enabled` to `false` to turn the LED off.

## Configuration

The defaults are compiled in, so the config store is optional. Keys are listed in
[config.hjson](config.hjson). They use the SDK names (`wlan.ssid`, `wlan.password`,
`wlan.security`, `wlan.pmf_mode`, `wlan.country_code`, `wlan.bssid`, `wlan.ap_bssid`,
`wlan.ap_beacon_interval_tus`, `wlan.ap_dtim_period`, `wlan.max_stas`,
`wlan.relay_depth_override`, `wlan.scan_selected_channels`, `wlan.selective_scan_attempts`,
`wlan.sta_scan_interval_*`, `ip.ip_addr`, `ip.netmask`, `ip.gateway`, `ip.hostname`,
`ip.dhcp_enabled`), plus `lab.*` keys for this firmware. A key that is missing, empty or
malformed leaves the default in place. Values out of range are logged and ignored, including an
`ip.*` address that is not dotted-quad, a `wlan.country_code` that is not in the regulatory
database, and a `wlan.max_stas` of 0 or above the `MAX_STAS` build value (the heap and packet
pools are sized for it, so more stations need a rebuild with `MAX_STAS=N`).

The relay STA's reconnect scan backoff defaults to 2 s growing to a 10 s cap
(`LAB_STA_SCAN_INTERVAL_BASE_S` / `LAB_STA_SCAN_INTERVAL_LIMIT_S` in `halow_lab_config.h`, the
same as node A), instead of the SDK's 512 s cap. With the SDK default, a relay that lost node B
for a few minutes could wait several more minutes before its next scan, and node A cannot
associate while the relay AP is down.

All three nodes must agree on the SSID, security, passphrase, PMF, country and channel list.
The relay has no channel setting, because it follows the root.

## Design notes

**Which MAC is the relay's IP address on?** mmipal's lwIP netif binds to the first VIF that
comes up, which is the STA VIF here. So `192.168.50.2` resolves to the **device (STA) MAC**,
printed as `sta_mac` in `ev=boot`, not to the relay AP BSSID `02:48:4C:00:00:02`. Use
`sta_mac` for any static ARP entry on node A or node B.

**#7 (VIF inference).** This does not affect the relay's own traffic. mmnetif sets
`metadata.vif` explicitly to its bound VIF (the STA VIF). For frames addressed to a station
associated with the relay AP, or to a node further down, `umac_relay_update_tx_metadata()`
rewrites the VIF to the AP VIF. Everything else goes upstream. In the receive direction, a
unicast frame from node A to the relay's STA MAC arrives on the AP VIF and is handed to the STA
VIF netif (umac_datapath.c, the `RA == AP BSSID && DA == own STA MAC` case). A <-> relay unicast
therefore works without the "Unable to infer VIF" error.

**Broadcasts on the repeater (#10 note).** A broadcast from node A (for example its ARP request
for 192.168.50.2) is forwarded upstream only. The relay does not deliver it locally when it
arrives on the AP. It sees the request when the root re-broadcasts it, and it receives that copy
on its STA VIF. The relay's own broadcasts (for example its ARP request for node A) go up to the
root as well. When the root re-broadcasts them, the relay forwards them down to its AP and does
not deliver them to itself. ARP between A and the relay therefore needs a working root. That is
acceptable, because the relay's netif link only follows the upstream STA link anyway: when the
root is gone, the link is down, and after `lab.relay_recovery_timeout_ms` the relay AP is torn
down too. The optional `lab.arpN.ip`/`lab.arpN.mac` entries install static ARP entries on every
link-up. Use them if hardware testing shows the MM6108 drops frames that carry its own SA, which
has not been verified.

**No DHCP.** The lab uses static addresses only, which avoids depending on broadcasts from the
root.

## SDK bugs and what the relay relies on

| Bug | Effect on the relay | Handling |
|---|---|---|
| #9 use after free in `umac_ap_remove_sta_record()` | The relay AP calls into relay code with a freed STA record every time node A disassociates, and pending ADDBA, RX reorder or defrag timeouts can fire on the freed record afterwards; either can crash the relay | Patch `0001` is **required**. There is no safe app-level workaround. |
| #10 root group-addressed frames | Affects the root (node B), not the repeater | Patch `0002`, which has no effect on the relay |
| #11 `passphrase_len = strlen(ssid)` | The relay STA would use a truncated SAE password | The app always sets `passphrase_len`. Patch `0003` fixes the SDK. |
| #7 VIF inference | Not hit, because mmnetif always sets the VIF | Nothing needed |

`build.sh` applies the patches by default. Build with `--revert-patches` to leave the submodule
clean afterwards.

## Status

The firmware builds cleanly with `-Werror -Wall -Wextra`, both with and without the patches.

First hardware run (2026-11-08, EKH05 with MM6108A1, firmware 2.1.1, on the same
desk): drag-and-drop flashing works, the relay joins node B with SAE in about 3 s from boot (RSSI
about -30 dBm), reaches `state=repeater,depth=1`, answers ping and UDP echo from B (RTT p50
13-14 ms), and rejoins B by itself after B reboots (link down, AP removed after the 10 s recovery
timeout, rescan, rejoin about 4 s after B's root AP is back). Results:
[docs/results/2026-11-08-root-relay.md](../docs/results/2026-11-08-root-relay.md). With node A
behind it, the relay forwarded UDP, TCP and ping between A and B for 11 minutes with no loss, and
A rejoined through it 3.4 s after the relay's AP came back from a reboot:
[docs/results/2026-11-08-a-relay-b.md](../docs/results/2026-11-08-a-relay-b.md).
