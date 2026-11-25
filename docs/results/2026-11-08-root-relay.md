<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# 2026-11-08: node B (root) and relay on hardware

First run on the boards. Only one ESP32-S3 was available, so it ran **node B** and the EKH05 ran
the relay. Node A was not tested. Traffic came from node B's diagnostic probes
(`CONFIG_LAB_B_PING_RELAY=y`, `CONFIG_LAB_B_UDP_ECHO_RELAY=y`), so this covers the B ↔ relay hop
only.

## Setup

| | Node B | Relay |
|---|---|---|
| Board | Seeed XIAO ESP32-S3 (chip rev v0.2, 8 MB flash) + XIAO WM6108 | MM6108-EKH05 |
| Chip / firmware | MM6108 (chip ID 0x0306), Morse firmware 2.1.1, BCF `mf16858` | MM6108A1 (0x0306), firmware 2.1.1, BCF `mm6108-mf08651-us` |
| Software | node_b at `ffd5b0b` + root to relay probes, ESP-IDF 6.0.2, esp-halow `3c46c92`, patches 0001-0003 | relay_ekh05 at `ffd5b0b` + BCF/IP-counter fixes, mm-iot-sdk 2.14.3 + patches 0001-0003 |
| Flashing | `idf.py flash` over USB Serial/JTAG | drag-and-drop onto `/Volumes/STLINK_V3P` (ST-LINK `0483:374F`, firmware V3J8M3B5S1) |

Radio settings: the lab defaults (AU, REVmf channel list, S1G channel 37, 920.5 MHz, 2 MHz, op class
51, SAE with PMF required). Both boards were on the same desk, connected to the same laptop.

## Results

| Check | Result |
|---|---|
| Relay boot | Pass. AU regdb found (26 channels), channel 37 in the list, chip booted, `relay_enable` status 0. |
| Node B boot | Pass. `root_up` on channel 37, op class 51; BSSID = device MAC with the locally administered bit set. |
| Relay joins B (SAE, PMF) | Pass. Associated and authorized on B, relay `state=repeater,depth=1`, RSSI -27 to -32 dBm. 3.3 s from B's root AP up (relay already scanning), 3.3 s from relay boot (B already up). |
| B → relay ping (56 B, 1/s) | 114 of 115 before the reboot test; the one loss is the first ping, during ARP. RTT min/avg/max 9 / 14 / 20 ms. |
| B → relay UDP echo (64 B, 10/s) | 1148 of 1150 before the reboot test; both losses (and one `tx_err`) in the first second, during ARP. RTT avg 13.0 ms, p50 14.3 ms, p95 19.9 ms, jitter 2.6 ms. The relay's server saw 1142 of 1143 requests. |
| Broadcast from the root (#10) | Pass: B resolved the relay's MAC by ARP, so group-addressed frames leave the root. |
| B reboots (root loss) | Pass. Relay link down at once, AP removed after the 10 s recovery timeout (`state=repeater` → `sta`), rescans, rejoins 3.6 s after B's root AP is back. |
| Relay reboots under traffic | Pass, no crash on B (patch 0001 path). B keeps the stale association until the relay re-associates (`state=gone` then `authorized`); exactly the ~22 s of traffic sent during the outage is lost (218 echoes, 22 pings), none after. One `Packet failed to send - 9` on B at re-association. |
| Stability | 4 min of continuous traffic after the reboot test: 0 further loss, heap flat (B 240 KB free, relay 60 KB), no dropped events. |

Raw logs and the `parse_logs.py` output were kept locally (`logs/`, `bringup/`), not committed.

## Fixed during the run

- Relay `ev=boot` printed the BCF description raw. The EKH05 BCF describes itself as JSON, which
  broke the comma-separated line. It now prints the module name (`bcf=mm6108-mf08651-us`).
- Relay `ip_tx_pkts`/`ip_rx_pkts` were always 0, because lwIP statistics are off in the SDK
  build. The relay Makefile now sets `LWIP_STATS=1` (+0.6 KB RAM).
- Node B gained the optional root to relay probes used here.

## Observations to follow up

- With no root on the air, the relay reported `sta_state=disabled` (not `connecting`) in its
  health lines for the first minute, although it joined within seconds once B came up.
- The ESP boards have 8 MB flash; the 4 MB layout causes a harmless boot warning.

## Next

Node A through the relay ([test-plan.md](../test-plan.md) T1 onwards). That needs a second ESP32-S3
+ WM6108, or the same one re-flashed as node A while another root is available.
