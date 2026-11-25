<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# node_b: S1G relay root AP

Node B is a Seeed XIAO ESP32-S3 with a Seeed XIAO WM6108 (MM6108). It is the **root AP** of
the lab's S1G relay network. Node A reaches it through the EKH05 relay:

```
  node_a (STA) ~~~ relay (EKH05, repeater) ~~~ node_b (root AP, this project)
  192.168.50.10    192.168.50.2                192.168.50.1
                   AP BSSID 02:48:4C:00:00:02
```

When it boots, node B:

1. Loads the AU channel list (802.11 REVmf) and gives the HaLow netif the static address
   `192.168.50.1/24`. It runs no DHCP client or server, and has no gateway.
2. Starts the root AP with `mmhalow_relay_enable()` and `root_ap_args`: SSID `halow-relay-lab`,
   WPA3-SAE with PMF required, S1G channel 37 (2 MHz, 920.5 MHz) and global operating class 51.
   At most 4 stations can associate directly.
3. Runs the lab servers from `common/`: the UDP echo server on port 5000 and the TCP message
   stream server on port 5001. They run for the node's whole uptime.
4. Prints EVENT lines for station status (MAC, AID, state), relay status and link changes, plus a
   health line every 10 s.

B only answers traffic. It learns each peer's ARP entry from that peer's ARP request, so it never
needs to broadcast.

## Prerequisites

- ESP-IDF v6.0.2: `source ~/.espressif/tools/activate_idf_v6.0.2.sh`.
- The esp-halow submodule at `third_party/esp-halow` (branch `sdk-2.14.3-idf6`, which has
  `CONFIG_HALOW_RELAY_MODE` and `mmhalow_relay_enable()`). `main/idf_component.yml` points
  `morsemicro/halow` at `../../third_party/esp-halow/halow`.
- The mm-iot-sdk patches applied to esp-halow's SDK copy: run `tools/apply_sdk_patches.sh` from the
  repository root, and run it again after every `git submodule update`.
  - **0001 (#9) is required.** Without it, the root crashes (use after free) every time a station
    disassociates.
  - 0002 (#10) lets a relay root send group-addressed frames. On node B it is only defence in
    depth: esp-halow (`3c46c92`) already works around #10 in its TX path by giving the root's
    group frames an RA, so the patched branch never runs here and B can send ARP requests with
    or without it. See `patches/README.md`, 0002 section.

## Build, flash, monitor

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
cd node_b
idf.py build                       # target esp32s3 comes from sdkconfig.defaults
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

`sdkconfig.defaults` is only read when `sdkconfig` is created. After you edit it, delete
`sdkconfig` (or run `idf.py set-target esp32s3`). To change settings, use `idf.py menuconfig`:
**HaLow lab: network plan** and **HaLow lab: node B (relay root)**.

`sdkconfig.defaults` contains:

- The XIAO ESP32-S3 + WM6108 pins and BCF `bcf_fgh100mhaamd.bin`, the same as esp-halow's
  `configs/sdkconfig.defaults.seeed_xiao_esp32s3-seeed_xiao_mm6108`.
- `CONFIG_HALOW_COUNTRY_CODE="AU"`, `CONFIG_WIFI_MORSE_AU_CHANNELISATION_REVMF=y`,
  `CONFIG_HALOW_AP_MODE=y` and `CONFIG_HALOW_RELAY_MODE=y`.
- 40 TX and 40 RX packet blocks, as in the SDK relay_mode application.
- `CONFIG_FREERTOS_HZ=1000`, the lwIP options from the esp-halow softap example, a 2 MB factory
  partition, and the console on USB Serial/JTAG (the XIAO's USB-C port).

The build stops with an error if the AU channelisation is not REVmf. At runtime, the node refuses
to start the radio if `CONFIG_LAB_COUNTRY` differs from `CONFIG_HALOW_COUNTRY_CODE`.

## Configuration

| Option | Default | Notes |
|---|---|---|
| `CONFIG_LAB_SSID` / `CONFIG_LAB_PASSPHRASE` | `halow-relay-lab` / `change-me-halow-au` | The passphrase is a placeholder. Change it on all three nodes. |
| `CONFIG_LAB_COUNTRY` | `AU` | Must equal `CONFIG_HALOW_COUNTRY_CODE`. |
| `CONFIG_LAB_S1G_CHAN_NUM` / `CONFIG_LAB_OP_CLASS` | 37 / 51 | In the REVmf list, `s1g_channels_AU[23..48]`, this is the entry {920.5 MHz, global 51, S1G 23, ch 37, 2 MHz}. morselib accepts 51 or 23. |
| `CONFIG_LAB_PRI_BW_MHZ` / `CONFIG_LAB_PRI_1MHZ_CHAN_IDX` | 2 / 0 | |
| `CONFIG_LAB_ROOT_AP_BSSID` | empty (device MAC) | Optional fixed root BSSID. |
| `CONFIG_LAB_IP_NODE_B` / `_RELAY` / `_NODE_A`, `CONFIG_LAB_NETMASK`, `CONFIG_LAB_GATEWAY` | `.1` / `.2` / `.10`, `/24`, `0.0.0.0` | |
| `CONFIG_LAB_UDP_ECHO_PORT` / `CONFIG_LAB_TCP_STREAM_PORT` | 5000 / 5001 | |
| `CONFIG_LAB_REPORT_INTERVAL_MS` | 5000 | Server STATS period. |
| `CONFIG_LAB_B_MAX_STAS` | 4 | Stations that associate directly. Nodes behind the relay do not count. |
| `CONFIG_LAB_B_FORWARDING_TABLE_SIZE` | 0 (SDK default 64) | |
| `CONFIG_LAB_B_UDP_ECHO_SERVER` / `CONFIG_LAB_B_TCP_STREAM_SERVER` | y / y | |
| `CONFIG_LAB_B_TCP_IDLE_TIMEOUT_MS` | 60000 | |
| `CONFIG_LAB_B_HEALTH_INTERVAL_MS` | 10000 | |
| `CONFIG_LAB_B_STATIC_ARP` + `_NODE_A_MAC` / `_RELAY_MAC` | n | Static ARP fallback, see below. |

### Static ARP fallback

With `CONFIG_LAB_B_STATIC_ARP=y`, B installs static ARP entries `192.168.50.10 -> node A MAC` and
`192.168.50.2 -> relay MAC`. An entry is skipped if its MAC is empty. B installs them on every link
up, every time a station reaches `authorized`, and on every health interval. lwIP drops all ARP
entries, static ones included, when the netif goes down.

- Node A's MAC is in its boot line, `EVENT,node,...,ev=boot,...,mac=...`.
- For the relay, use its **device (STA) MAC**, not the AP BSSID `02:48:4C:00:00:02`. The relay's
  IP stack is bound to its STA interface.

You need this only for a build where the root cannot broadcast: issue #10 without patch 0002 and
without esp-halow's RA workaround. It also mitigates the second part of issue #9: an existing
station's ARP entry lost when another station associates. That has no confirmed root cause in the
SDK or in esp-halow. The two plausible ESP-IDF causes are the DHCP server's temporary static entries
and `netif_set_down()` clearing the ARP table. This node avoids both, because it runs no DHCP server
and the netif stays up while the root AP is up.

### Root to relay probes (diagnostic)

With `CONFIG_LAB_B_PING_RELAY=y` and/or `CONFIG_LAB_B_UDP_ECHO_RELAY=y` (menu **Root to relay
probes**, both off by default), B pings the relay and runs a UDP echo client against the relay's
echo server, from the first time a station is authorized until reset. This tests the root <->
relay hop on its own, for example with only B and the relay powered. The lines are the same as
node A's: `PING,relay,...` and `STATS,udp_relay,...`, plus
`EVENT,svc,ev=start,ping_relay=,udp_relay=,dst=`. The relay's echo server serves one client at a
time, so do not also run node A's `udp_relay` flow.

## Output

The traffic services print STATS and EVENT lines, which are specified in
[common/README.md](../common/README.md). The node itself adds the EVENT lines below, where `t_ms`
is milliseconds since boot. Parse lines with `^(STATS|EVENT),` anywhere in a line, because they
can be mixed with `I (...) tag: ...` log lines.

```
EVENT,node,t_ms=812,ev=boot,app=node_b,role=root,mac=xx:xx:xx:xx:xx:xx,ip=192.168.50.1,netmask=255.255.255.0,country=AU,static_arp=0
EVENT,wlan,t_ms=1630,ev=root_up,bssid=xx:xx:xx:xx:xx:xx,ssid=halow-relay-lab,chan=37,op_class=51,pri_bw_mhz=2,max_stas=4
EVENT,relay,t_ms=1631,ev=status,state=root,depth=0
EVENT,wlan,t_ms=1640,ev=link_up,down_ms=1640
EVENT,udp_srv,t_ms=0,ev=listening,proto=udp,port=5000
EVENT,tcp_srv,t_ms=0,ev=listening,proto=tcp,port=5001
EVENT,ap,t_ms=20511,ev=sta,mac=<relay STA MAC>,aid=1,state=associated,stas=0
EVENT,ap,t_ms=20530,ev=sta,mac=<relay STA MAC>,aid=1,state=authorized,stas=1
EVENT,udp_srv,t_ms=31002,ev=session,peer=192.168.50.10:xxxxx,session=1a2b3c4d,first_seq=0
STATS,udp_srv,t_ms=36002,sent=50,rcvd=50,lost=0,...,peer=192.168.50.10:xxxxx,session=1a2b3c4d,bad=0,tx_err=0,final=0
EVENT,tcp_srv,t_ms=31500,ev=accepted,peer=192.168.50.10:xxxxx,accepts=1
EVENT,node,t_ms=40000,ev=health,uptime_s=40,heap_free=...,heap_min=...,role=root,link=1,stas=1,stas_known=1,evt_drops=0
```

| Line | Meaning |
|---|---|
| `ev=root_up` | The root AP is running. `chan`/`op_class` come from `mmwlan_get_vif_channel_info()`. |
| `EVENT,ap,ev=sta` | A station directly associated with the root changed state (`authenticated`, `associated`, `authorized`, `gone`). `stas` counts authorized stations. Node A sits behind the relay, so the only station here is the relay, unless A is pinned to the root for a direct baseline. |
| `EVENT,relay,ev=status` | morselib relay status callback (role, depth). |
| `ev=link_up` / `ev=link_down` | The netif link, which follows the root AP interface. `down_ms` / `up_ms` give how long the previous state lasted. |
| `EVENT,arp,ev=static` / `static_fail` | Static ARP fallback. `err` is the lwIP `err_t` (`-4` = ERR_RTE, no route yet). |
| `EVENT,svc,ev=exit` | A server returned an error and is restarted after `retry_ms`. |
| `ev=health` | Uptime, free and minimum heap, link state, and authorized / known station counts. `evt_drops` counts morselib events lost because the queue was full. |
| `EVENT,node,ev=fatal` | Startup failed (`country_mismatch`, `static_ip_failed`, `bad_root_bssid`, `relay_enable_failed`). The node halts. |
