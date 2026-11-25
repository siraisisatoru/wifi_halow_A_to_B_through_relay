<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# Hardware bring-up

Bring the nodes up in this order: **B** (root), then the **relay**, then **A**. Each step has
a log line that must appear before you go on. Keep a console open on every board, with host
timestamps, so that [sim/tools/parse_logs.py](../sim/tools/parse_logs.py) can put the three logs
on one time axis afterwards.

The machine-readable lines (`EVENT`, `STATS`, `PING`) are described in
[common/README.md](../common/README.md), [node_a/README.md](../node_a/README.md#output),
[node_b/README.md](../node_b/README.md#output) and
[relay_ekh05/README.md](../relay_ekh05/README.md#console-output).

## 0. Prepare

```sh
git clone https://github.com/siraisisatoru/wifi_halow_A_to_B_through_relay.git
cd wifi_halow_A_to_B_through_relay
tools/bootstrap.sh --docker      # submodules, SDK patches, Docker images
tools/apply_sdk_patches.sh --check
```

Every line of the check must say `applied`. `mkdir -p logs` for the console captures below
(`logs/` is ignored by git). Then:

- Set your own passphrase for all three nodes in one place: `cp lab_local.conf.example
  lab_local.conf` at the repository root and set `CONFIG_LAB_PASSPHRASE` (and the SSID if you
  like). The file is git-ignored. Node A and node B read it as sdkconfig defaults, so delete a
  node's existing `sdkconfig` after changing it (the build warns if it is stale). The relay
  compiles it in. A `wlan.password` written to the relay's config store would still override it.
- Keep the boards about 1 m apart for the first run. Placing them for real relay distances
  comes later ([test-plan.md](test-plan.md), T6).

## 1. Node B (root AP)

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
cd node_b
idf.py build
idf.py -p /dev/cu.usbmodemB flash
idf.py -p /dev/cu.usbmodemB monitor --timestamps \
    --timestamp-format "%Y-%m-%d %H:%M:%S.%f" | tee ../logs/node_b.log
```

Expect, within a few seconds:

```
EVENT,node,...,ev=boot,app=node_b,role=root,mac=<B MAC>,ip=192.168.50.1,...,country=AU
EVENT,wlan,...,ev=root_up,bssid=<B BSSID>,ssid=halow-relay-lab,chan=37,op_class=51,pri_bw_mhz=2,max_stas=4
EVENT,relay,...,ev=status,state=root,depth=0
EVENT,wlan,...,ev=link_up,...
EVENT,udp_srv,...,ev=listening,proto=udp,port=5000
EVENT,tcp_srv,...,ev=listening,proto=tcp,port=5001
```

Write down B's `mac` and `bssid`. You need the BSSID for the direct baseline (T5) and the MAC
for static ARP.

| Symptom | Check |
|---|---|
| `ev=fatal,reason=country_mismatch` | `CONFIG_LAB_COUNTRY` and `CONFIG_HALOW_COUNTRY_CODE` must both be `AU`. |
| `ev=fatal,reason=relay_enable_failed` | The `mmhalow_relay_enable failed: <error>` log line just before it. `ESP_ERR_NOT_SUPPORTED` means `CONFIG_HALOW_RELAY_MODE` is off. |
| No `root_up`, chip errors at boot | The WM6108 is seated and the pins in `sdkconfig.defaults` match the XIAO. The BCF is `bcf_fgh100mhaamd.bin`. |
| `chan` is not 37 | REVmf channelisation (`CONFIG_WIFI_MORSE_AU_CHANNELISATION_REVMF=y`). |

## 2. Relay (EKH05)

```sh
relay_ekh05/build.sh                     # or: relay_ekh05/build.sh bin-with-config
```

Flash it as described in [relay_ekh05/flash.md](../relay_ekh05/flash.md): on macOS, copy
`relay_ekh05/build/relay_ekh05.bin` onto the ST-LINK drive (`/Volumes/STLINK_V3P` on the board
tested so far). The board restarts on its own after the copy.

Open the console (115200 8N1, `screen` or `cu`, see flash.md) and expect:

```
EVENT,relay,...,ev=regdb,country=AU,found=1,...,lab_chan=37,...
EVENT,relay,...,ev=boot,...,sta_mac=<relay STA MAC>
EVENT,relay,...,ev=config,ssid=halow-relay-lab,security=sae,pmf=required,passphrase_len=<N>,...,ap_bssid=02:48:4c:00:00:02,...
EVENT,relay,...,ev=relay_enable,role=repeater,status=0
EVENT,relay,...,ev=sta_state,state=connected,bssid=<B BSSID>,rssi_dbm=...
EVENT,relay,...,ev=relay_status,state=repeater,sta_state=connected,depth=1,parent=<B BSSID>
EVENT,relay,...,ev=link,state=up,ip=192.168.50.2,...
```

On node B at the same time:

```
EVENT,ap,...,ev=sta,mac=<relay STA MAC>,aid=1,state=associated,stas=0
EVENT,ap,...,ev=sta,mac=<relay STA MAC>,aid=1,state=authorized,stas=1
```

The LED should show 1 green pulse (depth 1) and then 0 blue pulses (no stations yet).

While no root is on the air, the relay can stay at `sta_state=disabled` in its health lines
instead of `connecting`; it is still scanning.

To check the B ↔ relay hop on its own (no node A needed), build node B with
`CONFIG_LAB_B_PING_RELAY=y` and `CONFIG_LAB_B_UDP_ECHO_RELAY=y`
([node_b/README.md](../node_b/README.md#root-to-relay-probes-diagnostic)). Expect
`PING,relay,...,ev=reply` lines on B and `EVENT,udp_srv,ev=session,peer=192.168.50.1:...` on
the relay. The first ping and a couple of UDP echoes are lost while ARP resolves.

| Symptom | Check |
|---|---|
| `passphrase_len` in `ev=config` is not the length of your passphrase | Patch 0003 and the config store value of `wlan.password`. |
| `sta_state` loops between `connecting` and `disabled` | Passphrase or security mismatch with B. B shows `authenticated` but never `authorized`. |
| Relay authorized on B, then the relay or B crashes when the relay leaves | Patch 0001 is not applied on that node. |
| Solid red LED with `ev=error` | The `what` field: `set_channel_list` (country code), `mmwlan_boot` (chip or BCF) or `mmwlan_relay_enable`, with the SDK status in `code`. |

## 3. Node A

```sh
cd node_a
idf.py build                  # default: plain STA, pinned to the relay AP
idf.py -p /dev/cu.usbmodemA flash
idf.py -p /dev/cu.usbmodemA monitor --timestamps \
    --timestamp-format "%Y-%m-%d %H:%M:%S.%f" | tee ../logs/node_a.log
```

Expect:

```
EVENT,node,...,ev=boot,app=node_a,role=plain_sta,...,pin=relay,bssid_pin=02:48:4C:00:00:02
EVENT,wlan,...,ev=sta_state,state=connected,bssid=02:48:4c:00:00:02
EVENT,wlan,...,ev=link_up,bssid=02:48:4c:00:00:02,peer=relay,via_relay=1,...
EVENT,svc,...,ev=start,udp=1,udp_relay=0,tcp=1,ping_node_b=1,ping_relay=1
EVENT,tcp,...,ev=connected,attempt=1,...
PING,relay,...,ev=reply,dst=192.168.50.2,...
PING,node_b,...,ev=reply,dst=192.168.50.1,...
STATS,udp,...,sent=...,rcvd=...,lost=0,...
```

### Confirming that A goes through the relay

All of these must hold:

1. A's `ev=link_up` has `bssid=02:48:4c:00:00:02,peer=relay,via_relay=1`.
2. The relay logs `ev=ap_sta_join,mac=<A MAC>`, and its LED shows 1 green pulse, then 1 blue
   pulse.
3. Node B has **no** `EVENT,ap,ev=sta` line with A's MAC. Its only station is the relay
   (`stas=1`).
4. On node B, `EVENT,udp_srv,ev=session,peer=192.168.50.10:...` and
   `EVENT,tcp_srv,ev=accepted,peer=192.168.50.10:...` show A's traffic arriving.
5. `PING,relay` RTT is clearly lower than `PING,node_b` RTT: the second hop adds airtime. In the
   simulation the ratio was about 1 : 2.

Building node A with `CONFIG_A_MODE_RELAY_STA=y` also logs
`EVENT,relay,...,ev=status,...,depth=2`, which proves the path from the relay's side as well.

| Symptom | Check |
|---|---|
| A stays in `connecting` | The relay AP is up only while the relay is connected to B (step 2). The BSSID pin must match `wlan.ap_bssid`. |
| `peer=root` or `peer=other` | The pin is not set (`CONFIG_LAB_A_PIN_NONE`) or points elsewhere. |
| Link up, but every ping and UDP echo is lost | ARP. Check B's `EVENT,arp` lines and patch 0002. As a fallback, set static ARP on A and B with the MACs written down above. |
| Ping to B works, ping to the relay doesn't | The relay's own broadcasts go upstream only ([known-issues.md](known-issues.md)). Set `CONFIG_LAB_A_STATIC_ARP_RELAY_MAC` to the relay's `sta_mac`. |

## 4. Collect the logs

```sh
python3 sim/tools/parse_logs.py -o bringup/run1 logs/node_a.log logs/node_b.log logs/relay.log
docker run --rm -v "$PWD":/work -e MPLCONFIGDIR=/tmp/m halow-hostsim:1 \
    python3 sim/tools/plot_results.py bringup/run1
```

`parse_logs.py` needs only the Python standard library. The plots run in the
`halow-hostsim:1` image, so nothing is installed on the host. Then run the cases in
[test-plan.md](test-plan.md).
