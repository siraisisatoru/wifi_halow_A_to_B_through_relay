<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# Known issues and limitations

Issue numbers refer to the public issue tracker of
[MorseMicro/esp-halow](https://github.com/MorseMicro/esp-halow/issues), where these mm-iot-sdk
bugs were reported (esp-halow bundles mm-iot-sdk). The full analysis of
each SDK bug, and what to check when the SDK is bumped, is in
[patches/README.md](../patches/README.md).

## Status

All three nodes have run on the boards: B and the relay on their own
([results/2026-11-08-root-relay.md](results/2026-11-08-root-relay.md)), then the full A → relay → B
chain with UDP, TCP and ping, and recovery from a relay reboot
([results/2026-11-08-a-relay-b.md](results/2026-11-08-a-relay-b.md)). Range, capacity, soak and
the relay-STA variant are not tested yet.

| Part | Verified |
|---|---|
| Relay firmware | Builds in Docker with `-Werror`, with and without the patches. FLASH 931 KB, RAM 343 KB. |
| node_a, node_b | Build with ESP-IDF 6.0.2 against `third_party/esp-halow`, 0 warnings: node_a plain STA, relay STA and static ARP + root pin; node_b default and static ARP. |
| esp-halow fork | 23 example/target builds with IDF 6.0.2, 0 warnings |
| `common/` | 31 unit tests under ASan/UBSan |
| B ↔ relay on hardware | Association, SAE, ping and UDP echo, both reboot cases ([results](results/2026-11-08-root-relay.md)) |
| A → relay → B on hardware | T1, T2, T3, T7 pass ([results](results/2026-11-08-a-relay-b.md)); the other test-plan cases are not run yet |

## mm-iot-sdk 2.14.3 bugs

| Issue | Effect in this lab | Handling |
|---|---|---|
| #9 use after free in `umac_ap_remove_sta_record()` | The relay or node B can crash whenever a station leaves (for example A or the relay rebooting). Also stale ADDBA, reorder and defrag timeouts on any AP. | Patch 0001. Required on the relay and on node B. |
| #10 a relay root drops group-addressed frames | Node B cannot send ARP requests or other broadcasts, so it can reach A only while its ARP cache holds A. | Patch 0002 (SDK). esp-halow's TX glue also works around it on B. Static ARP on B (`CONFIG_LAB_B_STATIC_ARP`) is the fallback. |
| #11 `passphrase_len == 0` is replaced by `strlen(ssid)` | SAE uses the wrong passphrase length and the relay cannot join B. Because the relay AP copies the STA's credentials, A is then locked out too. | Patch 0003, and every node sets `passphrase_len` explicitly. esp-halow fills it in for relay STAs. |
| #7 "Unable to infer VIF ID" when both VIFs are valid | Wrong VIF for TX on a repeater. | Fixed in the esp-halow fork (TX VIF set explicitly). The relay firmware's stack uses the STA VIF. |

Run `tools/apply_sdk_patches.sh --check` after every `git submodule update`. The relay build
refuses to link an unpatched SDK unless `ALLOW_UNPATCHED=1` is given.

## Relay behaviour (by design in the SDK)

- **The relay API is alpha** (`mmwlan.h`): it is not intended for production use and may change in
  the next SDK release.
- **The relay's own broadcasts go upstream only.** While the relay has no uplink, A and the relay
  cannot ARP for each other, so pinging the relay from A needs B to be up. Static ARP entries
  between A and the relay remove that dependency. If the MM6108 firmware drops frames whose SA is
  its own MAC, static ARP between A and the relay becomes necessary even with B up (not yet
  checked on hardware).
- **Losing the root takes A down after 10 s.** The relay keeps its AP for
  `recovery_timeout_ms` and then removes it, which disassociates A. A plain STA keeps its link
  up during those 10 s even though nothing beyond the relay is reachable.
- **The relay's IP address lives on its STA interface.** Static ARP for the relay must use its
  STA MAC (printed in its `ev=boot` line as `sta_mac`), not the AP BSSID.

## This repository

- Throughput: with esp-halow's original SPI path, A → relay → B topped out at about 3.1 Mbit/s
  UDP, limited by the ESP32-S3's SPI transport
  ([results/2026-11-08-throughput.md](results/2026-11-08-throughput.md)). With the SPI fast path
  it reaches 6.25 Mbit/s at 8 MHz, and the relay's airtime becomes the limit. Direct A ↔ B reaches
  about 7.8 Mbit/s UDP, the ESP32-S3's own ceiling. TCP stays at about 3 Mbit/s
  ([results/2026-11-11-spi-fast-path-and-direct.md](results/2026-11-11-spi-fast-path-and-direct.md)).

- The ESP nodes use the 4 MB flash layout from the esp-halow examples, but the XIAO ESP32-S3 has
  8 MB. This is harmless: the larger app is 1.48 MB of a 2 MB partition.
- In the plain-STA build node A logs `chan=0,op_class=0` in `ev=link_up`.
  `mmwlan_get_vif_channel_info()` is only linked when esp-halow is built with AP mode, which the
  relay-STA variant enables.
- The UDP echo server keeps one client session at a time. A second client replaces the first.
- The simulation's `relay_outage` scenario models an R–B black hole with every association kept
  up, not the relay losing B. TCP recovery times in the simulation come from the Linux stack;
  lwIP's timers are coarser, so expect slower reconnects on the boards. See
  [sim/README.md](../sim/README.md#hardware-differences-to-expect).
- The ESP nodes report `W spi_flash: Detected size(8192k) larger than the size in the binary
  image header(4096k)` at boot: the 4 MB layout above. Harmless.

## esp-halow fork (`sdk-2.14.3-idf6`)

- `.gitmodules` keeps the upstream relative URL `../mm-iot-sdk.git`. A plain recursive clone of
  the fork fails to fetch the SDK. `tools/bootstrap.sh` overrides the URL locally.
- `eloop_replenish_timeout` links to ESP-IDF's own wpa_supplicant symbol. Its caller is compiled
  out, so it is never called.
- `crypto_dh_*` and `wpas_dpp_*` are referenced from code the linker garbage-collects. They would
  fail to link if that code were ever enabled.
