<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# Throughput experiments

sdkconfig fragments for the bulk throughput test
([common/port/esp_idf/tput.h](../../common/port/esp_idf/tput.h)). Each fragment is added on top
of the node's own `sdkconfig.defaults`. Results:
[docs/results/2026-11-11-spi-fast-path-and-direct.md](../../docs/results/2026-11-11-spi-fast-path-and-direct.md).

| Fragment | Node | Role |
|---|---|---|
| `a_client_relay.conf` | A | A → B client, joins the relay |
| `a_client_direct.conf` | A | A → B client, joins node B directly |
| `a_sink_relay.conf` | A | B → A sink, joins the relay |
| `a_sink_direct.conf` | A | B → A sink, joins node B directly |
| `b_sink_{1,2,4,8}mhz.conf` | B | A → B sink on the 1/2/4/8 MHz test channel |
| `b_client_{1,2,4,8}mhz.conf` | B | B → A client on the 1/2/4/8 MHz test channel |

Channels (AU REVmf list): 1 MHz ch 36, 2 MHz ch 37, 4 MHz ch 39, 8 MHz ch 35. The relay follows
node B's channel, and node A scans, so only node B is rebuilt per bandwidth.

All fragments turn off node A's other traffic and raise the lwIP TCP window and send buffer to
32 KB. The `*_direct` fragments pin node A to node B's BSSID, which is not in the repository: set
`CONFIG_LAB_ROOT_AP_BSSID` (the `bssid=` value of node B's `ev=root_up` line) in the git-ignored
`lab_local.conf` at the repository root (see `lab_local.conf.example`). Without it node A stops
at boot with `reason=bad_root_bssid`. For a
direct run, power the relay off so it does not stay associated with B and beacon on the same
channel.

## Build and flash

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
cd node_b
idf.py -B build_tput -D SDKCONFIG=build_tput/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;../experiments/throughput/b_sink_8mhz.conf" \
    set-target esp32s3 build flash
```

Do the same for node A with an `a_*` fragment. Pair a client with the matching sink:

| Direction, path | Node A | Node B |
|---|---|---|
| A → B via relay | `a_client_relay` | `b_sink_<bw>` |
| B → A via relay | `a_sink_relay` | `b_client_<bw>` |
| A → B direct | `a_client_direct` | `b_sink_<bw>` |
| B → A direct | `a_sink_direct` | `b_client_<bw>` |

## Reading the results

Each test produces one line at the receiver:

```
EVENT,tput_srv,t_ms=...,ev=end,proto=udp,test=...,ms=20124,bytes=...,kbps=6243,pkts=...,lost=...,loss_pct=20.17
```

The sender prints `EVENT,tput,...,ev=end,...,kbps=...,send_err=...` with the rate its `send()`
accepted. Ignore results that end before the receiver's `ev=link_up` / `ev=root_up` after a
reflash: those are tests cut off by the reset.
