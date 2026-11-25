<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# Test plan

Run these after [bringup.md](bringup.md) has succeeded. Each case says what to configure, what
to do and what counts as a pass. The pass thresholds are first targets for an in-house lab with
the boards a few metres apart; they come from the host simulation
([sim/results/SUMMARY.md](../sim/results/SUMMARY.md)) and should be revised with the first real
numbers.

Capture every board's console as in [bringup.md](bringup.md#4-collect-the-logs) and keep each run
in its own `bringup/<case>` directory. `parse_logs.py` writes `summary.json` and a Markdown
summary with loss, RTT percentiles, TCP reconnects and recovery times per flow.

Unless a case says otherwise, all nodes use their default configuration: A is a plain STA pinned
to the relay, UDP echo 10 req/s × 64 B, TCP 5 msg/s × 256 B, and pings to B and to the relay every
1 s.

## Summary

| ID | Case | Duration | Main pass criterion |
|---|---|---|---|
| T1 | Path through the relay | 2 min | All five checks in bringup.md §3 hold |
| T2 | Steady state | 10 min | UDP loss < 1 %, 0 TCP reconnects |
| T3 | Per-hop latency | from T2 | `PING,relay` p50 < `PING,node_b` p50 |
| T4 | Relay UDP echo (A↔relay hop) | 10 min | `udp_relay` loss < 1 % |
| T5 | Direct baseline (A pinned to B) | 10 min | Numbers to compare with T2 |
| T6 | Range: A out of B's reach | 10 min | T2 criteria still met |
| T7 | Relay power cycle | 3 cycles | A recovers each time without a reboot |
| T8 | Node B reboot (root loss) | 3 cycles | Relay and A recover; no crash on any node |
| T9 | Node A reboot | 3 cycles | Relay and B keep running; A rejoins via the relay |
| T10 | Relay-aware STA variant | 10 min + 1 relay cycle | `depth=2`; link down < 2 s after B is lost |
| T11 | Capacity sweep | ~5 min | Loss < 1 % up to the knee; record the knee |
| T12 | Soak | ≥ 12 h | No crash, heap stable, UDP loss < 1 % |

## Cases

### T1: path through the relay

Bring up B, then the relay, then A. Pass when every check in
[bringup.md §3](bringup.md#confirming-that-a-goes-through-the-relay) holds.

### T2: steady state

Run 10 minutes after A's first `ev=link_up`.

Pass:
- `udp` loss < 1 %, with `dup` and `corrupt` both 0.
- `tcp`: `connects=1`, `disconnects=0`, `stalls=0` and `lost=0` in the last `STATS,tcp` line
  (`rcvd` = `sent` − `inflight`).
- Ping to B and to the relay: loss < 1 %.
- No `ev=link_down` on any node.

Record UDP RTT p50/p95/max, jitter, and the goodput from `summary.json`. The simulation's
relay_nominal case gave about 22 ms p50 RTT, but the radio settings are a guess, so use it only
as a sanity check.

### T3: per-hop latency

From the T2 logs, compare `PING,relay` with `PING,node_b`. Pass when the relay's p50 is clearly
lower. The difference approximates the relay↔B hop plus forwarding in the relay.

### T4: A↔relay hop on its own

Node A with `CONFIG_LAB_A_UDP_ECHO_RELAY=y`. Run 10 minutes. Pass: `udp_relay` loss < 1 %.
The relay's `STATS,udp_srv` shows the uplink loss on that hop. Compare it with T2 to see which hop
loses packets.

### T5: direct baseline

Node A with `CONFIG_LAB_A_PIN_ROOT=y` and `CONFIG_LAB_ROOT_AP_BSSID` set to B's BSSID (from B's
`ev=root_up`). The relay can stay on. Run 10 minutes.

Pass: A's `ev=link_up` shows `peer=root`. Record the same numbers as T2: through the relay, RTT
should be roughly twice the direct value and capacity (T11) roughly half.

### T6: range

Place A where it can hear the relay but not B. Check this with T5's build first: it should fail
to associate, or associate with RSSI below about −90 dBm. Then run T2 with the default build.

Pass: the T2 criteria. Record the RSSI from A's `ev=health` and from the relay's `ev=health`
(upstream).

### T7: relay power cycle

During T2 traffic, power the relay off for 30 s and then on again. Repeat three times, at least 2
minutes apart.

Expected on A (plain STA): the link stays up until A detects the lost AP, then `ev=link_down`,
services stop, A rescans (backoff up to 10 s), rejoins the relay once the relay has rejoined B,
and services restart with new sessions.

Pass:
- A rejoins via the relay every time, without a reboot.
- `tcp` reconnects once per cycle.
- Recovery time (relay `ev=link,state=up` to A's next `EVENT,svc,ev=start`) < 30 s.
- Node B logs the relay leaving and rejoining, and does not crash. A crash on B here points at
  patch 0001.

### T8: node B reboot (root loss)

During T2 traffic, reset node B (button or `idf.py monitor` Ctrl-T Ctrl-R). Repeat three times.

Expected: the relay's STA loses B. After `recovery_timeout_ms` (10 s) the relay takes its AP down,
which disconnects A. When B is back the relay rejoins, its AP returns, and A rejoins.

Pass:
- The relay logs `ev=ap_sta_leave,...,state=ap_down` about 10 s after its `sta_state` leaves
  `connected`.
- The relay, then A, rejoin every time, and no node crashes.
- B's servers accept A's new sessions.

Record the time from B's `ev=root_up` to A's `EVENT,svc,ev=start`.

### T9: node A reboot

During steady state, reset A three times.

Pass:
- The relay logs `ap_sta_leave` and then `ap_sta_join` for A each time, and does not crash. A
  crash on the relay here points at patch 0001.
- B's UDP server logs a new `ev=session`, and the TCP server accepts the new connection.

### T10: relay-aware STA variant

Build A with `CONFIG_A_MODE_RELAY_STA=y`. Run 10 minutes of T2, then one T8 cycle.

Pass:
- `EVENT,relay,ev=status,...,depth=2` on A.
- T2 criteria.
- During T8, A reports `ev=link_down` within 2 s of the relay losing B (the plain STA waits for
  the relay's 10 s recovery timeout), and recovers as in T8.

### T11: capacity sweep

Build A with `CONFIG_LAB_A_TCP_STREAM=n`, `CONFIG_LAB_UDP_PAYLOAD_LEN=1000` and
`CONFIG_LAB_A_PING_RELAY=n`. Run 60 s each at `CONFIG_LAB_UDP_RATE_PPS` = 10, 20, 40, 60, 80, 100
and 120 (one build per step, or change the rate with `idf.py menuconfig` between runs).

Record delivered echoes per second, loss and RTT p95 per step, and find the knee (the highest rate
with < 1 % loss). Repeat with T5's build for the direct case. The simulation predicted a relay
knee near 80 req/s and about twice that direct, but that assumed a fixed MCS without
aggregation.

### T12: soak

Default configuration, at least 12 hours (overnight).

Pass:
- No `ev=fatal`, `ev=fatal_error` or reboot on any node.
- `heap_min` in the `ev=health` lines stays flat after the first hour on all three nodes.
- UDP loss < 1 % over the whole run.
- `evt_drops` / `ev_dropped` stay 0.

## Reporting

For each run, keep the three raw logs, the `parse_logs.py` output and a note of the board
positions, firmware commits (`git rev-parse HEAD`, and `git submodule status`) and any Kconfig or
config store changes. Add a results table per case to `docs/results/` once real numbers exist.
