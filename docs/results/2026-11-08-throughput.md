<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# 2026-11-08: A → relay → B throughput per bandwidth

Bulk throughput from node A to node B through the EKH05 relay, measured at node B (the receiver),
for every S1G bandwidth in the AU REVmf channel list.

## Method

- Node A: `CONFIG_LAB_A_TPUT=y`, other traffic off. It alternates a 20 s TCP test (1460-byte
  writes) and a 20 s UDP test (1460-byte datagrams, sent as fast as the stack accepts them),
  with a 3 s pause after each ([common/port/esp_idf/tput.h](../../common/port/esp_idf/tput.h)).
- Node B: `CONFIG_LAB_B_TPUT_SERVER=y`; it counts payload bytes and lost datagrams and prints one
  `EVENT,tput_srv,ev=end` line per test. kbps = payload bits per second (TCP stream bytes or
  UDP payload, no headers), 1 Mbit/s = 1000 kbit/s.
- Both ESP nodes: lwIP TCP window and send buffer raised from 14.6/8.8 KB to 32 KB (with 14.6 KB
  the window alone caps TCP near 3.9 Mbit/s at the ~30 ms RTT of the two hops).
- Only node B was re-flashed per bandwidth: the relay copies the root's channel and node A
  scans the whole list. Primary channel 2 MHz (1 MHz for the 1 MHz channel), index 0.
- 5-6 rounds per bandwidth; the table gives the median and the best round. The first round after
  each change is included (it is the lowest at 2 MHz and 4/8 MHz TCP, while rate control settles).
- All boards on one desk: RSSI -15 to -39 dBm on both hops. esp-halow `7c58ee0`, project `de9328b`
  plus the throughput test.

## Results

| BW | Channel | Centre | Op class | UDP median (best) | UDP loss at B | TCP median (best) |
|---|---|---|---|---|---|---|
| 1 MHz | 36 | 920.0 MHz | 50 | 0.72 Mbit/s (0.83) | 6-11 % | 0.17 Mbit/s (0.25) |
| 2 MHz | 37 | 920.5 MHz | 51 | 2.17 Mbit/s (2.22) | 4-10 % | 1.14 Mbit/s (1.41) |
| 4 MHz | 39 | 921.5 MHz | 52 | 3.09 Mbit/s (3.11) | 0 % | 1.91 Mbit/s (1.94) |
| 8 MHz | 35 | 919.5 MHz | 53 | 3.09 Mbit/s (3.11) | 0 % | 1.98 Mbit/s (2.03) |

All rounds (receiver kbps):

| BW | UDP | TCP |
|---|---|---|
| 1 MHz | 830, 712, 700, 754, 689, 729 | 251, 166, 168, 207, 130, 155 |
| 2 MHz | 1331, 2040, 2215, 2171, 2214 | 258, 932, 1142, 1284, 1407 |
| 4 MHz | 3112, 3097, 3087, 3110, 3090, 3089 | 1622, 1943, 1902, 1908, 1922, 1914 |
| 8 MHz | 3084, 3101, 3107, 3088, 3092, 3093 | 1644, 2028, 1990, 1983, 1965, 1982 |

## Reading the numbers

- **Highest: about 3.1 Mbit/s UDP and 2.0 Mbit/s TCP, at 4 or 8 MHz.**
- **1 and 2 MHz are air-limited.** Node A offers more than the two hops can carry (the relay
  repeats every frame on the same channel, so each packet uses the air twice), and the excess is
  lost at the relay.
- **4 and 8 MHz hit a node A limit, not the radio.** At both, A's own sender only manages 3.1
  Mbit/s (its `send()` blocks, with no send errors) and B receives all of it, with 0 loss. Doubling
  the bandwidth from 4 to 8 MHz changes nothing. The cap is on the ESP32-S3 side of node A: the
  esp-halow TX path and the 40 MHz SPI link to the WM6108. A faster host interface or TX path, not
  more bandwidth, is the next step.
- **TCP is 55-65 % of UDP** from 2 MHz up: data and ACKs share the same air through both hops. At
  1 MHz TCP falls to about 25 % of UDP; the long airtime per frame makes RTT and losses large for
  TCP.
- 2 MHz TCP was still rising round by round (rate control); it may settle higher with longer runs.

## Bug found and fixed during the test

The first UDP flood (into node B before it had a sink) crashed node B: esp-halow's `halow_rx()`
released a received packet a second time after `esp_netif_receive()` had failed (tcpip input queue
full) and esp_netif had already freed it (heap assert "block already marked as free"). After the
panic restart, node B then looped on interrupt watchdog timeouts at boot because the WM6108 IRQ pin
was still configured as a level interrupt; it needed a power cycle. Both are fixed in esp-halow
`7c58ee0`. No crash on any node in the 17 minutes of floods after the fix.
