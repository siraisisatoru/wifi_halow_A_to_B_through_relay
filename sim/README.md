<!--
Copyright 2026 siraisisatoru
SPDX-License-Identifier: Apache-2.0
-->

# Host network simulation of A → relay → B

This runs the lab's traffic code (`common/`, the same C that runs on the ESP32-S3 nodes) on Linux
network namespaces in the `halow-hostsim` Docker image. A tc model of the 2 MHz HaLow channel and
the single-radio repeater sits between the namespaces.

It is used for three things:

- **Checking the services and tools before the hardware works.** This covers UDP loss
  accounting, TCP stall detection and reconnect, and the STATS/EVENT line formats.
- **Setting expectations for bring-up.** It gives the RTT of one hop compared with two, the cost
  of an outage, and where the shared channel saturates.
- **Reusing the parser and plots on real serial logs**, because the boards print the same lines.

The numbers come from a model, not from the boards. Results are in
[results/SUMMARY.md](results/SUMMARY.md).

## Quick start

```sh
sim/run_sim.sh                       # build, calibrate, run every scenario (about 10 min)
sim/run_sim.sh relay_outage          # one scenario (file name in sim/scenarios without .json)
sim/run_sim.sh --parse-only          # re-parse, re-plot and rewrite SUMMARY.md from existing logs
```

The script re-runs itself in `docker run --privileged halow-hostsim:1`, because network namespaces
and tc need privileges. Nothing is installed on the host. To build the image, run
`docker build -t halow-hostsim:1 docker/hostsim`.

## Layout

| Path | What |
|---|---|
| `host_apps/` | `node_a_host`, `node_b_host`, `relay_host`: the node apps built against `common/` with its POSIX port. The options mirror the `CONFIG_LAB_*` Kconfig names (`--udp-rate-pps`, `--tcp-payload-len`, `--report-interval-ms` and so on; `--help` lists them) |
| `scenarios/*.json` | Scenarios: topology, channel model, per-hop link parameters, events, traffic, expected results |
| `tools/netsim.py` | Builds the namespaces and the tc model, runs the apps, ping and tcpdump, and applies timed events |
| `tools/parse_logs.py` | Turns logs into `stats.csv`, `events.csv`, `ping.csv` and `summary.json`, and checks them against the scenario's `expect` list. Uses only the standard library and works on serial logs too |
| `tools/plot_results.py` | Draws `timeline.png`, `rtt_cdf.png` and `saturation.png` for each scenario, plus the comparison plots |
| `tools/make_summary.py` | Writes `results/SUMMARY.md` and runs the checks across scenarios |
| `results/<scenario>/` | Raw logs (`node_a.log`, `node_b.log`, `relay.log`, `ping_*.log`, `netsim.log`), the parsed files, `udp_rtt.csv.gz` (per-packet RTT), `meta.json`, `tc_stats.txt` and plots |

## What runs where

| Namespace | Address | Runs | On the hardware |
|---|---|---|---|
| `hlA` | 192.168.50.10/24 | `node_a_host`: UDP echo client to B (`udp`) and to the relay (`udp_relay`), TCP stream client to B (`tcp`); `ping` to B and to the relay; `tcpdump` (UDP 5000 only, used to get exact per-packet RTT) | XIAO ESP32-S3 STA, with the BSSID pinned to the relay |
| `hlR` | 192.168.50.2/24 on `br0` (MAC 02:00:00:00:00:02, standing in for the relay's STA MAC) | Linux bridge `ra`↔`rb` (the L2 relay), the airtime model, and `relay_host` (UDP echo server `udp_relay_srv`) | EKH05 repeater |
| `hlB` | 192.168.50.1/24 | `node_b_host`: UDP echo server (`udp_srv`) and TCP stream server (`tcp_srv`), one task each | XIAO ESP32-S3 relay root AP |

In the `direct` topology, `hlR` has no IP address and makes no transmissions of its own. It only
carries the shared-channel model, so A and B talk over one hop.

## Link model

```
 hlA eth0 ──netem(hop a_r)──►  ra ─┐ ingress: airtime     ┌─ egress: airtime ── netem(hop r_b + fwd) ──► hlB eth0
                                    └──────────► br0 ──────┘
 hlA eth0 ◄──netem(hop a_r + fwd)── ra ◄─ egress: airtime ◄─ br0 ◄─ ingress: airtime ◄─ rb ◄──netem(hop r_b)── hlB eth0
                          one shared queue: hlR air0 (netem rate 5.2 Mbit/s + 924 B per frame, 50 frames)
```

### Per hop and direction

The transmitter's egress has a netem qdisc. It applies:

- **Latency:** `delay_ms` 2 ms one way. This stands for host, driver and firmware latency, such as
  SPI to the MM6108, the MAC queues and lwIP.
- **Jitter:** `jitter_ms` 0.5 ms, using the paretonormal distribution.
- **Residual frame loss:** `loss_pct`, the loss left after MAC retries.
- **Relay forwarding delay:** `relay_fwd_delay_ms` (1 ms) is added on the relay's own
  transmissions. In mm-iot-sdk relay mode, a forwarded frame crosses SPI to the STM32U585, goes
  through `umac_relay` and crosses SPI back.

Each hop qdisc also has a 1 Gbit/s rate, which keeps the netem queue FIFO. 802.11 delivers in
order for each TID, so jitter must not reorder frames.

### Shared channel

There is one queue in `hlR` (`air0`), served at the PHY rate. Each frame adds a fixed overhead
there:

- **Fixed per-frame time:** 1350 µs, converted to bytes at the PHY rate.
- **MAC bytes:** the 4-address QoS header 32 B, LLC/SNAP 8 B, CCMP 16 B and FCS 4 B, minus the
  Ethernet header that is already counted.

Each over-the-air transmission passes through this queue once. That includes:

- Frames that the relay receives (sent by A or B).
- Frames that the relay transmits (forwarded, or its own).

So a relayed frame costs airtime twice, which is the store-and-forward halving of a single-radio
repeater. In `direct`, every frame costs airtime once.

How frames are steered through the queue:

1. `skbedit` marks each frame with its stage.
2. `mirred` redirects it to `air0`.
3. A filter on `air1` sends it back to the device and direction it came from.

The queue length (`queue_frames` 50) sets where frames are tail-dropped at saturation.

### Bug #10 model

`root_drops_group_tx` (on in every scenario) adds a u32 filter on hlB's egress. It drops any frame
whose destination address has the multicast bit set, as the unpatched mm-iot-sdk relay root does
(`umac_relay_update_tx_metadata()` sends group-addressed frames to the absent STA VIF). At setup,
one broadcast ping from B checks that the filter works. SUMMARY.md reports both the self-test
result and how many group-addressed frames B tried to send during the run.

### Events

`link_down` sets 100 % loss on both directions of a hop for `duration_s`. It black-holes the hop
while every interface, bridge and association stays up. That models an outage shorter than
beacon-loss detection, **not** the relay losing the root, which on hardware also takes the
relay's own IP and then its AP down (see "Hardware differences to expect"). `set` changes hop
parameters for a while.

The relay's MAC in the simulation is a placeholder. On the EKH05 the relay's IP and ARP address
is its STA (device) MAC, printed as `sta_mac` in the relay's `ev=boot` line (see
`relay_ekh05/README.md`, Design notes). `02:48:4C:00:00:02` is only the relay AP BSSID that node A
associates to; do not use it for a static ARP entry or a capture filter on the relay's IP.

### Timer calibration

Docker Desktop's VM fires hrtimers late. Each netem stage adds about 0.35 to 0.4 ms. `run_sim.sh`
measures this once (`results/calibration.json`) and subtracts it from each hop delay. The airtime
stages keep their extra delay, which adds about 0.8 ms to a direct RTT and 1.6 ms to a relayed
RTT.

### Radio assumptions

The S1G 2 MHz, 1 spatial stream PHY rates with long GI are, in Mbit/s:

| MCS | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| PHY rate | 0.65 | 1.3 | 1.95 | 2.6 | 3.9 | 5.2 | 5.85 | 6.5 | 7.8 |

Short GI is ×10/9, for example MCS7 is 7.2 Mbit/s. So "MCS 2..7" covers about 2.0 to 6.5 Mbit/s.
The scenarios use **MCS5, 5.2 Mbit/s**, a plausible rate for a bench lab.

The fixed time per frame adds up to 1350 µs:

| Part | Time (µs) |
|---|---|
| S1G short preamble: STF + LTF1 + SIG, 6 symbols of 40 µs | 240 |
| SIFS | 160 |
| NDP ACK | 240 |
| AIFS[BE] = SIFS + 3 × 52 µs slot | 316 |
| Mean backoff, CWmin 15 (7.5 slots) | 390 |
| **Total** | **1350** |

This gives a MAC efficiency of about 62 % for a 1500 B frame: 3.75 ms of airtime, or
3.2 Mbit/s. For the 64 B echo it gives only about 6 %, because 1.63 ms of airtime carries 138 B.
That matches the "about 50 to 60 % MAC efficiency" rule of thumb for large frames, and shows why
small packets are dominated by overhead.

To use a different MCS, change `medium.phy_rate_kbps`. The per-frame overhead is converted to
bytes automatically.

### Not modelled

- **Rate adaptation and A-MPDU aggregation.** The MM6108 supports A-MPDU, so bulk TCP on hardware
  can beat the per-frame numbers here.
- **Collisions and hidden nodes.** A and B never collide; the mean backoff stands in for
  contention.
- **Beacons and management airtime.**
- **Power save, RAW and TWT.**
- **Retries.** They show up only as residual loss and as jitter.
- **Linux TCP/ARP instead of lwIP.** See "Hardware differences to expect".

## Scenarios

| File | Topology | Change from nominal |
|---|---|---|
| `baseline_direct` | direct | A associated to B directly (one hop) |
| `relay_nominal` | relay | two clean hops (2 ms ± 0.5 ms each), 1 ms relay forwarding |
| `relay_lossy` | relay | 3 % loss per hop and direction, 2 ms jitter per hop |
| `relay_outage` | relay | R–B link black-holed for 15 s from t = 20 s |
| `relay_saturation` | relay | UDP echo with 1000 B payload, rate swept 10 → 140 requests/s, 8 s per step; TCP off; ping on |
| `direct_saturation` | direct | the same sweep at 20 → 280 requests/s, to compare capacity |

The normal traffic is:

- **UDP echo:** 10 requests/s, 64 B. The defaults are `LAB_UDP_RATE_PPS` and
  `LAB_UDP_PAYLOAD_LEN`.
- **TCP stream:** 5 messages/s, 256 B, with ACK timeout 5 s, connect timeout 5 s and backoff from
  0.5 s to 30 s.
- **Ping:** every 200 ms, 56 B. This is faster than the 1 s firmware default, to get more samples.
- **STATS lines:** every 1 s.

Each scenario lists its expected results under `expect`, for example `flows.udp.lost` between 140
and 175 for the outage. `parse_logs.py` evaluates them and SUMMARY.md lists the outcome.

## Results (run of 2026-11-03; outage and saturation scenarios re-run 2026-11-04)

The full tables and checks are in [results/SUMMARY.md](results/SUMMARY.md). All 21 scenario checks,
the 6 bug #10 filter self-tests and the 3 cross-scenario checks passed.

- **Latency.** The UDP echo RTT p50 is 7.8 ms direct and 22.3 ms through the relay, and the A→relay
  hop alone is 11.6 ms. With the other traffic switched off, a relayed small echo costs
  4 × (2 ms + 1.63 ms airtime) + 2 × 1 ms forwarding, about 16.5 ms. A separate run with no
  other traffic measured 18.2 ms, which includes about 1.6 ms of timer artefact. The extra 4 ms in `relay_nominal` is queueing behind node A's own
  TCP, ping and relay-echo frames on the shared channel. Those sources start together and stay
  phase-locked.
- **Capacity.** The relay delivers 80 echo/s of 1000 B, which is 640 kbit/s of payload in each
  direction. Direct delivers 160 echo/s, or 1.28 Mbit/s each way. The ratio is exactly 0.50, and
  both are within 2 % of the model capacity (81 and 163 echo/s).
  - Near the knee, RTT jumps from about 30 ms to the full-queue value: 4 × 50 frames × 3.07 ms,
    about 600 ms through the relay, and about 300 ms direct.
  - Past the knee, the delivered rate falls as more airtime goes to requests that are dropped
    later.
  - Ping to B loses 10.6 % over the whole sweep through the relay (14.0 % direct), all of it
    from the knee step onwards. Ping now runs until the last, heaviest step has finished.
- **Loss accounting** matches the model:
  - In `relay_lossy`, round-trip loss is 11.9 % (1 − 0.97⁴ = 11.5 %), uplink loss is 7.3 %
    (1 − 0.97² = 5.9 %), and loss to the relay is 4.8 %.
  - TCP delivered every message, with p95 RTT 376 ms from retransmissions and no stall.
- **R-B black hole (15 s, `relay_outage`, association held):**
  - UDP lost exactly 150 requests (10 requests/s × 15 s), all on the uplink, and the longest gap
    between echoes was 15.1 s. `udp_relay` lost nothing, but only because the simulated relay
    stays associated and up; on hardware a real loss of the root takes the relay's IP down too
    (see "Hardware differences to expect").
  - TCP detected the stall 5.05 s after the link went down (`ack_timeout_ms` 5000) and gave up the
    25 messages in flight, which are counted as `lost` (the client's `ev=end` reports them as
    `abandoned=25`).
  - One connect attempt timed out (`err=110`, 5 s connect timeout). The next attempt connected
    0.67 s after the link came back, when its SYN retransmission got through. That delay depends
    on timing; see below.
  - `tcp_srv` adopted the new connection, and its session and sequence numbers continued. This
    too depends on the association being held.
- **Bug #10:** B sent **zero** group-addressed frames in every scenario, while the self-test showed
  the drop filter works. B only ever needed unicast. It learns A's MAC address from A's broadcast
  ARP request, and its later neighbour probes are unicast. So "B only answers" is enough with the
  unpatched root, at least with Linux ARP; check this with lwIP (see below).

## Hardware differences to expect

- **Losing the root is not the `relay_outage` black hole.** In `relay_outage` the relay stays
  associated, so `udp_relay` and ping to the relay keep working and TCP resumes its session. On
  the boards, when the relay misses node B's beacons long enough (firmware beacon-loss
  detection), mm-iot-sdk 2.14.3 does this:
  1. The relay STA disconnects and the relay netif goes down with it (its link follows the
     upstream STA), so `udp_relay` and ping to the relay are lost from then on.
  2. After `recovery_timeout_ms` (10 s, `lab.relay_recovery_timeout_ms`),
     `umac_relay_stop_relay_ap()` tears down the relay AP and node A is disassociated.
  3. Node A, with `LAB_A_STOP_SERVICES_ON_LINK_DOWN=y` (the default), stops UDP, TCP and ping.
  4. Once B is back, the relay re-scans (scan backoff 2 s growing to 10 s), runs SAE and restarts
     its AP. Node A then re-scans with its own backoff, associates, waits
     `LAB_A_SERVICE_START_DELAY_MS` and starts its clients again as **new** sessions. TCP
     therefore opens a new session rather than continuing the old one, and the recovery time is
     scan + association + start delay on both hops, not the 0.67 s seen in `relay_outage`.

- **TCP timers.** lwIP's TCP timers are coarser than Linux's. Its initial RTO, which also times
  SYN retransmissions, is 1.5 to 3 s depending on the build (`CONFIG_LWIP_TCP_RTO_TIME` in
  ESP-IDF). Linux uses a 200 ms minimum RTO and 1 s for SYN. So on the boards:
  - Reconnect after an outage takes longer and depends more on timing.
  - Recovery after link-up falls between the next SYN retransmission and
    `connect_timeout_ms` + the backoff step, which is up to a few seconds with the defaults.
  - Lossy links cause more stalls.
- **ARP on node B (lwIP).** lwIP refreshes an ARP entry that is in use before `ARP_MAXAGE` runs out
  (300 s by default), with a unicast request 30 s before expiry and a broadcast 15 s before. Under bug #10 the
  broadcast is dropped. If A misses the unicast refresh, B's entry expires and B can't answer A
  until A ARPs again. esp-halow `3c46c92` already avoids this on node B: its TX glue gives the
  root's group-addressed frames an RA, so the broadcast goes out (patch 0002 is the SDK-side
  fix; see `patches/README.md`).
- **Rate control and aggregation.** These change capacity in both directions. Read the saturation
  numbers as a lower bound for MCS5 without A-MPDU.

## Using the tools on real boards

`parse_logs.py` recognises lines anywhere in a line, so ESP-IDF monitor output with `I (1234) tag:`
log lines mixed in is fine. For a common time axis, capture each board with host timestamps:

```sh
idf.py -p /dev/cu.usbmodemA monitor --timestamps --timestamp-format "%Y-%m-%d %H:%M:%S.%f" | tee node_a.log
idf.py -p /dev/cu.usbmodemB monitor --timestamps --timestamp-format "%Y-%m-%d %H:%M:%S.%f" | tee node_b.log
python3 sim/tools/parse_logs.py -o bringup/run1 node_a.log node_b.log relay.log
docker run --rm -v "$PWD":/work -e MPLCONFIGDIR=/tmp/m halow-hostsim:1 \
    python3 sim/tools/plot_results.py bringup/run1
```

`parse_logs.py` needs only the Python standard library. The plots need matplotlib, which is in
the Docker image or can go in `.venv` (`python3 -m venv .venv && .venv/bin/pip install matplotlib`).
Recognised prefixes:

- `[12.345678] ` (seconds)
- `YYYY-MM-DD HH:MM:SS[.ffffff] `, with or without brackets

Without a prefix, each flow's own `t_ms` is used.

Recognised ping output:

- iputils
- `esp_ping`-style lines: `64 bytes from 192.168.50.1 icmp_seq=3 ttl=64 time=12 ms` and
  `From 192.168.50.1 icmp_seq=4 timeout`
- Node A's machine lines (`node_a/main/ping_probe.c`). With `CONFIG_LAB_A_PING_VERBOSE=n` only
  the cumulative summaries are printed, and the parser then reports their totals and
  min/avg/max (no percentiles):

```
PING,node_b,t_ms=<ms>,ev=reply,dst=<ip>,seq=<n>,rtt_ms=<ms>,ttl=<n>,size=<n>
PING,node_b,t_ms=<ms>,ev=timeout,dst=<ip>,seq=<n>
PING,node_b,t_ms=<ms>,ev=summary,dst=<ip>,sent=<n>,rcvd=<n>,...,rtt_min_ms=..,rtt_avg_ms=..,rtt_max_ms=..
```

- A generic machine format with microsecond RTTs:

```
PING,<flow>,t_ms=<ms>,dst=<ip>,seq=<n>,rtt_us=<us>[,ttl=<n>]
PING,<flow>,t_ms=<ms>,dst=<ip>,seq=<n>,status=timeout
```

Ping flows are named after the target: `ping_node_b`, `ping_relay` or `ping_<ip>`. A machine
flow name without the `ping_` prefix gets it (node A's `node_b` becomes `ping_node_b`).

A client flow that ran more than once in one log (node A stops its clients on link down and
starts new runs, with `t_ms` back at 0, on link up) is reported as one flow combining all runs:
counters are summed, RTT min/max/avg are exact, percentiles come from the summed histograms,
and `runs`, `restarts` and `sessions` are added. For TCP, `reconnects` is the total connects
minus one. Each run is still listed under `segments`. Without a pcap,
STATS percentiles are interpolated from the 12-bucket histogram and clamped to the exact
min and max (`rtt_pct_source: "hist"`). The simulation gets exact percentiles from the capture
(`"pcap"`).
