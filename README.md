# wifi_halow_A_to_B_through_relay

A demonstration of Wi-Fi HaLow (802.11ah) **S1G relay mode**, built from open-source material and
Seeed XIAO boards. Node A reaches node B only through a relay:

```
  node A  ~~~ S1G ~~~  relay  ~~~ S1G ~~~  node B
  XIAO ESP32-S3        MM6108-EKH05        XIAO ESP32-S3
  + XIAO WM6108        relay repeater      + XIAO WM6108
  STA                  (STA up, AP down)   relay root AP
  192.168.50.10        192.168.50.2        192.168.50.1
```

The lab runs in Australia (AU regulatory domain, S1G channel 37 at 920.5 MHz, 2 MHz). A sends
UDP echo traffic, a TCP message stream and pings to B, and every node prints machine-readable
statistics.

**Status:** the full A → relay → B chain runs on the boards: no loss in steady state, RTT p50
about 34 ms end to end, and recovery after a relay reboot in under 10 s
([docs/results/2026-11-08-a-relay-b.md](docs/results/2026-11-08-a-relay-b.md)). Range, capacity
and soak tests are still to do; see [docs/known-issues.md](docs/known-issues.md).

## Contents

| Path | What |
|---|---|
| [node_a/](node_a/) | Node A firmware (ESP-IDF 6, esp-halow). Plain STA pinned to the relay; optional relay-aware STA. |
| [node_b/](node_b/) | Node B firmware (ESP-IDF 6, esp-halow). S1G relay root AP with the UDP echo and TCP stream servers. |
| [relay_ekh05/](relay_ekh05/) | Relay firmware for the MM6108-EKH05, built with mm-iot-sdk 2.14.3's make flow in Docker (no STM32Cube tools). |
| [common/](common/) | Shared protocol, statistics and traffic services for all nodes and the simulation, with unit tests. |
| [patches/mm-iot-sdk/](patches/mm-iot-sdk/) | Fixes for mm-iot-sdk 2.14.3 bugs reported as esp-halow issues [#9](https://github.com/MorseMicro/esp-halow/issues/9), [#10](https://github.com/MorseMicro/esp-halow/issues/10) and [#11](https://github.com/MorseMicro/esp-halow/issues/11), applied by [tools/apply_sdk_patches.sh](tools/apply_sdk_patches.sh). |
| [sim/](sim/) | Host network simulation of A–relay–B (Linux network namespaces, in Docker) and the log parser, which also reads real board logs. Results in [sim/results/](sim/results/SUMMARY.md). |
| [docker/](docker/) | `ekh05-builder` (Arm GCC + OpenOCD) and `hostsim` images. |
| [third_party/](third_party/) | Submodules: [mm-iot-sdk](https://github.com/MorseMicro/mm-iot-sdk) 2.14.3 and [esp-halow](https://github.com/siraisisatoru/esp-halow/tree/sdk-2.14.3-idf6) (a fork ported to ESP-IDF 6 with mm-iot-sdk 2.14.3 and relay root support). |
| [docs/](docs/) | [Architecture](docs/architecture.md), [bring-up](docs/bringup.md), [test plan](docs/test-plan.md), [known issues](docs/known-issues.md). |

## Quick start

Requirements: git, Docker, and ESP-IDF v6.0.2 for the ESP nodes. Every other tool runs inside a
Docker image.

```sh
git clone https://github.com/siraisisatoru/wifi_halow_A_to_B_through_relay.git
cd wifi_halow_A_to_B_through_relay
tools/bootstrap.sh --docker          # submodules, SDK patches, Docker images
cp lab_local.conf.example lab_local.conf   # then set your SSID and passphrase (git-ignored)

relay_ekh05/build.sh                 # relay firmware -> relay_ekh05/build/

source ~/.espressif/tools/activate_idf_v6.0.2.sh
(cd node_b && idf.py build)
(cd node_a && idf.py build)

sim/run_sim.sh                       # optional: rerun the host simulation
```

Then follow [docs/bringup.md](docs/bringup.md): node B first, then the relay, then node A.

Before using the lab, set your own passphrase in `lab_local.conf`, instead of the placeholder
`change-me-halow-au`. That one git-ignored file sets the SSID, passphrase and node B's BSSID for
all three firmwares, so none of them goes into git (see
[lab_local.conf.example](lab_local.conf.example)).

The SSID, the relay AP BSSID `02:48:4C:00:00:02` and the IP plan are made up for this lab, not
taken from any device. They are listed in
[docs/architecture.md](docs/architecture.md#values-made-up-for-this-lab).

## License

The files in this repository carry `SPDX-License-Identifier: Apache-2.0` headers. The submodules
keep their own licences.
