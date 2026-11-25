<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# 2026-11-11: SPI fast path, both directions, relay vs direct

Follow-up to [2026-11-08-throughput.md](2026-11-08-throughput.md), which found node A's ESP32-S3
capped at about 3.1 Mbit/s by esp-halow's SPI transport. This run measures:

1. A → relay → B after the SPI fast path (and a 240 MHz CPU);
2. B → relay → A, to check symmetry;
3. A ↔ B directly, without the relay (B = AP, A = STA), in both directions.

## What changed since 2026-11-08

- **esp-halow SPI fast path** (`CONFIG_MM_SPI_FAST_PATH`, `components/shims/mmhal_wlan.c`).
  morselib's SDIO-over-SPI transport makes many single-byte transfers per packet (bus-ready polls,
  tokens, CRC bytes). Each one used to be a complete ESP-IDF SPI master transaction with DMA set-up,
  costing far more than the byte itself. The driver now only configures the bus once. Every
  transfer then writes the SPI data registers directly, in CPU mode, 64 bytes per hardware transfer,
  as the mm-iot-sdk STM32 platforms do. SPI clock unchanged at 40 MHz (`CONFIG_MM_SPI_CLOCK_MHZ`,
  the highest exact ESP clock below the MM6108's 50 MHz limit).
- **CPU at 240 MHz** instead of 160 MHz on both ESP nodes.
- **Throughput test in both directions:** node B gained a client (`CONFIG_LAB_B_TPUT_CLIENT`) and
  node A a sink (`CONFIG_LAB_A_TPUT_SERVER`). The per-test build settings are in
  [experiments/throughput/](../../experiments/throughput/).

These changes were measured from the working tree: esp-halow `7c58ee0` plus the uncommitted fast
path, and this repository at `c31b6e3` plus uncommitted changes.

## Method

- Same as 2026-11-08: 20 s TCP test (1460-byte writes), 3 s pause, 20 s UDP test (1460-byte
  datagrams, sent as fast as the stack accepts them), repeated. Throughput is payload bits per
  second **measured at the receiver**. lwIP TCP window and send buffer are 32 KB on both nodes.
- Channels (AU, REVmf list, primary 2 MHz except 1 MHz): 1 MHz ch 36 (920.0 MHz, op class 50),
  2 MHz ch 37 (920.5, 51), 4 MHz ch 39 (921.5, 52), 8 MHz ch 35 (919.5, 53). Only node B is
  re-flashed per bandwidth. The relay copies B's channel, and node A scans.
- **Via relay:** node A pinned to the relay AP `02:48:4C:00:00:02`.
- **Direct:** the EKH05 relay was unplugged. Node A was pinned to node B's BSSID
  (`peer=root,via_relay=0`), and B listed A as its own station.
- Rounds: 6 per protocol for relay A → B, 4 for the other cases. A result is only counted after
  the receiving node's link came up following the change, which drops tests cut off by a reflash.
- All three boards were on the same desk, connected to the same laptop. RSSI was -15 to -30 dBm
  via the relay and -22 dBm direct, so this is not a range test.

## Results

Medians in Mbit/s, UDP / TCP, measured at the receiver:

| BW (ch) | Relay A→B, 8 Nov (old SPI, 160 MHz) | Relay A→B | Relay B→A | **Direct A→B** | **Direct B→A** |
|---|---|---|---|---|---|
| 1 MHz (36) | 0.72 / 0.17 | 0.80 / 0.22 | 0.70 / 0.15 | **1.90 / 0.50** | **0.91 / 0.16** |
| 2 MHz (37) | 2.17 / 1.14 | 2.32 / 1.50 | 2.03 / 1.40 | **4.92 / 2.87** | **4.67 / 2.85** |
| 4 MHz (39) | 3.09 / 1.91 | 4.48 / 2.36 | 4.78 / 2.34 | **7.18 / 3.12** | **7.79 / 2.94** |
| 8 MHz (35) | 3.09 / 1.98 | 6.25 / 2.54 | 6.70 / 2.40 | **6.23 / 2.98** | **7.81 / 3.10** |

UDP loss at the receiver:

| BW | Relay A→B | Relay B→A | Direct A→B | Direct B→A |
|---|---|---|---|---|
| 1 MHz | 8–10 % | 5–11 % | 1–2 % | 3–7 % |
| 2 MHz | 2–5 % | 1–14 % | 0 % | 0 % |
| 4 MHz | 15–23 % | 2–12 % | 0 % | 0 % |
| 8 MHz | 20–29 % | 3–4 % | 0 % | 0 % |

### All rounds (kbit/s at the receiver; UDP loss % in brackets)

**Relay A→B** (node B sink):

| BW | TCP | UDP |
|---|---|---|
| 1 MHz | 224, 127, 228, 207 | 814 (8.5), 801 (7.8), 801 (9.6), 764 (8.1) |
| 2 MHz | 1536, 1652, 1604, 1468, 1401, 1396 | 2322 (4.7), 2314 (4.5), 2311 (4.6), 2341 (5.2), 2312 (4.5), 2378 (1.9) |
| 4 MHz | 2160, 2398, 2390, 2087, 2381, 2345 | 4727 (15.4), 4581 (18.0), 4516 (18.9), 4356 (19.2), 4281 (21.0), 4435 (22.8) |
| 8 MHz | 2272, 2321, 2538, 2534, 2564, 2546 | 5788 (29.4), 6343 (19.8), 6282 (19.5), 6156 (21.3), 6243 (20.2), 6258 (19.5) |

At 1 MHz, two later rounds (TCP 168, 123; UDP 480 (39.0), 594 (16.7)) are left out. Node B had
restarted just before them (see Observations).

**Relay B→A** (node A sink):

| BW | TCP | UDP |
|---|---|---|
| 1 MHz | 199, 138, 100, 157 | 741 (5.2), 703 (7.5), 637 (9.5), 689 (10.7) |
| 2 MHz | 1068, 1507, 1542, 1291 | 2018 (2.2), 2032 (2.2), 2028 (1.3), 1974 (13.6) |
| 4 MHz | 2245, 2336, 2334, 2353 | 4896 (1.9), 4847 (2.3), 4627 (11.5), 4714 (10.6) |
| 8 MHz | 2099, 2430, 2409, 2379 | 6729 (3.7), 6339 (3.2), 6725 (3.2), 6667 (3.5) |

**Direct A→B** (node B sink):

| BW | TCP | UDP |
|---|---|---|
| 1 MHz | 371, 519, 483, 569 | 1839 (2.4), 1844 (2.3), 1948 (1.2), 1956 (1.4) |
| 2 MHz | 2506, 2891, 2846, 2900 | 4970, 4789, 4861, 5080 (all 0) |
| 4 MHz | 2876, 3176, 3100, 3137 | 7182, 7199, 7169, 7168 (all 0) |
| 8 MHz | 2850, 2983, 2970, 3000 | 6222, 6233, 6276, 6211 (all 0) |

**Direct B→A** (node A sink):

| BW | TCP | UDP |
|---|---|---|
| 1 MHz | 236, 94, 143, 170 | 762 (3.0), 682 (4.9), 1149 (6.9), 1059 (7.2) |
| 2 MHz | 2577, 2921, 2856, 2826 | 4399, 4638, 4700, 4715 (all 0) |
| 4 MHz | 2919, 2953, 2934, 2936 | 7799, 7788, 7788, 7765 (0–0.01) |
| 8 MHz | 3008, 3097, 3104, 3098 | 7819, 7814, 7804, 7791 (all 0) |

### Sender-side UDP rate

How fast the sending node's `send()` accepted data. When it is close to the received rate, the
sender (or its radio queue) is the limit. When it is higher, the path dropped the difference.

| Case | Sender rate | Received |
|---|---|---|
| Relay A→B, 8 MHz | A 7.8–8.2 Mbit/s | 6.25 (relay airtime limit, loss at the relay) |
| Relay A→B, 4 MHz | A 5.4–5.8 Mbit/s | 4.48 |
| Direct B→A, 4 and 8 MHz | B 7.8 Mbit/s | 7.8 (no loss: B's transmit path is the limit) |
| Direct A→B, 8 MHz | A 6.2 Mbit/s | 6.2 (no loss: A's transmit queue drained slower) |
| Direct B→A, 1 MHz | B 0.75–0.8 Mbit/s (first rounds) | about the same |

## Findings

1. **The SPI fast path removes the 3.1 Mbit/s host cap.** Via the relay at 8 MHz, A now offers
   about 8 Mbit/s and B receives 6.25 Mbit/s UDP (2×). TCP rises 25–30 % at every bandwidth. With
   the fast path the ESP32-S3's own ceiling is about **7.8 Mbit/s UDP**, seen in direct B→A at both
   4 and 8 MHz.
2. **Through the relay, the air is now the limit at every bandwidth.** The relay repeats each
   frame on the same channel, so loss appears at the relay once the offered load exceeds what two
   hops can carry.
3. **Relay mode is about symmetric.** B→A is within 10–15 % of A→B at every bandwidth.
4. **Removing the relay roughly doubles throughput where the air was the limit.** At 2 MHz, UDP
   goes from 2.0–2.3 to 4.7–4.9 Mbit/s with 0 % loss, and TCP from 1.4–1.5 to 2.9 Mbit/s. At
   4–8 MHz, direct UDP hits the ESP's 7.8 Mbit/s ceiling.
5. **TCP plateaus at about 3 Mbit/s** in direct mode from 2 MHz up, far below UDP. That points to
   TCP processing or ACK timing on the ESP32 nodes, not the air. It's the next tuning target.
6. **Direct mode is not symmetric at 1 and 8 MHz.** At 1 MHz A→B gets 1.9 Mbit/s and B→A 0.9; at
   8 MHz A→B gets 6.2 and B→A 7.8. In both cases the slower sender's own transmit queue drains
   slower, with no loss. Likely causes are different rate-control choices per direction and, on the
   wide 8 MHz channel (915.5–923.5 MHz), interference from other users of the band. Neither is
   confirmed; the radio's rate statistics are not logged yet.
7. **1 MHz is limited by per-frame overhead.** Even direct, B → A at 1 MHz carried less than
   1 Mbit/s. TCP is especially poor at 1 MHz (0.15–0.5 Mbit/s).

## Observations

- **Unexplained restart of node B** during the relay A→B 1 MHz run (22:32:51). Its USB serial port
  disappeared for 3 s and B came back from a reset. There was no panic, watchdog or brownout message
  before it, and the reset reason was lost while the port was gone. It did not recur in about
  55 minutes of further floods. Printing the reset reason at boot would identify it if it happens
  again.
- **No SPI errors.** Across about 75 minutes of floods with the fast path, A and B logged no
  `Bus not ready`, CMD53 timeout, CRC, panic or assert lines.
- **The unplugged relay stayed in node B's station list** (`stas=2`) until its association timed
  out. It carried no traffic.

## Next

- Commit the SPI fast path to the esp-halow fork and the node changes here.
- Investigate the TCP plateau (lwIP task core and priority, delayed ACK, window scaling).
- Log the radio's rate-control statistics, to explain the direct-mode asymmetry.
- Range test (test-plan T6), with the boards apart.
