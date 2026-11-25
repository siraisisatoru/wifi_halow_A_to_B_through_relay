<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# Architecture

Three Wi-Fi HaLow (802.11ah, MM6108) nodes in an in-house lab in Australia. Node A reaches node B
only through an S1G relay running on an MM6108-EKH05 board.

```
  node A                         relay                            node B
  XIAO ESP32-S3 + WM6108         MM6108-EKH05 (STM32U585)         XIAO ESP32-S3 + WM6108
  ESP-IDF 6 + esp-halow          mm-iot-sdk 2.14.3 (bare make)    ESP-IDF 6 + esp-halow
  plain STA (or relay STA)       relay repeater: STA up + AP down relay root AP

  192.168.50.10  ~~~ S1G ~~~  [AP 02:48:4C:00:00:02 | STA] ~~~ S1G ~~~  192.168.50.1
                               192.168.50.2 (on the STA side)
  UDP echo client  ───────────────────────────────────────────▶  UDP echo server :5000
  TCP stream client ──────────────────────────────────────────▶  TCP stream server :5001
  ping B, ping relay           UDP echo server :5000 (A↔relay hop only)
```

| Node | Hardware | Firmware | Role |
|---|---|---|---|
| A | Seeed XIAO ESP32-S3 + Seeed XIAO WM6108 | [node_a](../node_a/) | Station. Generates all lab traffic. Pins the relay AP BSSID. |
| Relay | Morse Micro MM6108-EKH05 | [relay_ekh05](../relay_ekh05/) | S1G relay repeater: a STA towards B and an AP towards A on one radio. |
| B | Seeed XIAO ESP32-S3 + Seeed XIAO WM6108 | [node_b](../node_b/) | S1G relay root AP. Runs the UDP echo and TCP stream servers. |

## Radio plan

All three nodes use the same settings. The defaults live in
[common/include/halow_lab_config.h](../common/include/halow_lab_config.h) and can be changed
through Kconfig (A, B) or the config store (relay, [config.hjson](../relay_ekh05/config.hjson)).

| Setting | Value |
|---|---|
| Country | `AU` |
| Channel list | 802.11 REVmf channelisation for AU (the mm-iot-sdk default; on the ESP nodes `CONFIG_WIFI_MORSE_AU_CHANNELISATION_REVMF`) |
| Channel | S1G channel 37, 2 MHz, centre 920.5 MHz, primary 2 MHz |
| Operating class | global 51 (S1G 23) |
| SSID | `halow-relay-lab` |
| Security | WPA3-SAE with PMF required. The passphrase `change-me-halow-au` is a placeholder; change it on all three nodes. |

Only node B chooses the channel. The relay AP follows the channel of the root it joined, and node A
scans for the relay.

## Why node A pins the relay BSSID

The mm-iot-sdk relay AP has no SSID of its own: it re-advertises the root's SSID, security and
passphrase. Node A therefore sees two BSSs called `halow-relay-lab` and, if left alone, would
normally pick B when B is in range, which bypasses the relay. So:

- The relay AP uses a fixed, locally administered BSSID `02:48:4C:00:00:02`
  (`relay_ap_args.bssid`, config store key `wlan.ap_bssid`).
- Node A sets `mmwlan_sta_args.bssid` to that value (`CONFIG_LAB_A_PIN_RELAY`, the default). For
  a direct A↔B baseline, `CONFIG_LAB_A_PIN_ROOT` pins B's BSSID instead.

## Addressing

All three nodes are on one IPv4 subnet, `192.168.50.0/24`, with static addresses and no gateway:
B `.1`, relay `.2`, A `.10`.

- The S1G relay forwards frames at layer 2 (its forwarding table maps A's address to the relay),
  so A and B are on the same broadcast domain. No routing is involved.
- No node runs a DHCP server on HaLow, and the nodes have nothing upstream to route to. Static
  addresses keep bring-up independent of DHCP across the relay, which builds on the AP and relay
  APIs that mm-iot-sdk marks as alpha (`mmwlan.h`).
- The relay's IP stack is bound to its STA interface, so the relay is reachable only while its
  uplink to B is up. A static ARP entry needs the relay's **STA MAC**, not `02:48:4C:00:00:02`.
- Both ESP nodes can install static ARP entries for the case where broadcasts do not reach them
  (see [known-issues.md](known-issues.md), #10).

## Values made up for this lab

The identifiers below were chosen for this lab. None of them comes from a real device or network,
and any of them can be changed for another setup.

| Value | What it is |
|---|---|
| `02:48:4C:00:00:02` | Relay AP BSSID. The relay firmware assigns it; it is not burned into any hardware. The first byte, `0x02`, marks a locally administered address, which no manufacturer assigns. `48:4C` is ASCII "HL" (HaLow), and the last byte matches the relay's `.2` address. If two labs built from this repository share a channel within range of each other, change it in one of them and keep the `0x02` first byte. |
| `halow-relay-lab` | SSID. |
| `change-me-halow-au` | Placeholder passphrase. Set your own in `lab_local.conf`. |
| `192.168.50.0/24` (B `.1`, relay `.2`, A `.10`) | Private (RFC 1918) subnet. |
| `10.99.0.1`, `10.99.0.2` | Private addresses used only by the host simulation's delay calibration ([sim/tools/netsim.py](../sim/tools/netsim.py)). |
| `xx:xx:xx:xx:xx:xx` | Stands in for a real board address in docs and example logs. |

Real board identifiers are never committed: WM6108 MACs, BSSIDs derived from them (such as node
B's), ESP32 MACs and USB serial numbers. Node B's BSSID goes in the git-ignored `lab_local.conf`,
and raw logs stay in the git-ignored `logs/` and `bringup/`.

## Traffic

| Flow | From → to | What it measures |
|---|---|---|
| `udp` | A → B:5000 UDP echo, 10 req/s, 64 B | End-to-end loss (split into uplink and downlink), RTT, jitter, duplicates, reordering |
| `udp_relay` (optional) | A → relay:5000 UDP echo | The A↔relay hop on its own |
| `tcp` | A → B:5001 framed message stream, 5 msg/s, 256 B, application ACKs | Stalls, reconnects and recovery time after link loss |
| `ping_node_b`, `ping_relay` | A → B and A → relay, ICMP, 1/s | RTT per hop. Comparing the two separates the A↔relay hop from the relay↔B hop. |

The wire format, the console line formats (`STATS`, `EVENT`, `PING`) and the TCP framing are
specified in [common/README.md](../common/README.md). The same `common/` code runs on both ESP
nodes, on the relay (mmosal port) and in the host simulation (POSIX port).

## Software

| Part | Where | Notes |
|---|---|---|
| mm-iot-sdk 2.14.3 | [third_party/mm-iot-sdk](../third_party/mm-iot-sdk) (upstream, pristine) | Used by the relay build |
| esp-halow | [third_party/esp-halow](../third_party/esp-halow) (fork, branch `sdk-2.14.3-idf6`) | ESP-IDF 6 port of esp-halow with mm-iot-sdk 2.14.3 and S1G relay root support (`CONFIG_HALOW_RELAY_MODE`, `mmhalow_relay_enable()`). Contains its own copy of mm-iot-sdk 2.14.3. |
| SDK patches | [patches/mm-iot-sdk](../patches/mm-iot-sdk) | Fixes for the mm-iot-sdk bugs reported as esp-halow issues [#9](https://github.com/MorseMicro/esp-halow/issues/9), [#10](https://github.com/MorseMicro/esp-halow/issues/10) and [#11](https://github.com/MorseMicro/esp-halow/issues/11), applied to both SDK trees by [tools/apply_sdk_patches.sh](../tools/apply_sdk_patches.sh). The submodules themselves stay pristine. |
| Shared code | [common/](../common/) | Protocol, statistics, traffic services, platform ports, unit tests |
| Relay build | [docker/ekh05-builder](../docker/ekh05-builder) | Ubuntu 22.04 + xPack Arm GCC 14.2.1 + OpenOCD 0.12. No STM32CubeIDE or CubeMX. |
| Simulation | [sim/](../sim/), [docker/hostsim](../docker/hostsim) | Linux network namespaces A–R–B with tc netem, running the `common/` code |

## Relay behaviour that shapes the design

- The relay AP comes up only after the relay STA has associated with B, and the relay's IP link
  follows the STA.
- If the relay loses B, it keeps its AP for `recovery_timeout_ms` (10 s) and then tears it
  down, which disassociates A. A plain STA cannot tell "relay up, root gone" from "all good"
  until that happens. The relay-STA variant of node A (`CONFIG_A_MODE_RELAY_STA`) learns the loss
  of the root path straight away from the relay status.
- Scan backoff is capped at 10 s on A and on the relay (the SDK default of 512 s would leave a
  node off the air for minutes after an outage).
