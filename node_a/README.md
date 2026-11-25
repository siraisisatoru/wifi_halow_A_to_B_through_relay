<!--
Copyright 2026 siraisisatoru

SPDX-License-Identifier: Apache-2.0
-->

# node_a: HaLow STA behind the relay

Node A is a Seeed XIAO ESP32-S3 with a Seeed XIAO WM6108 (MM6108). It is a HaLow station that
must reach node B (the root AP) **through** the EKH05 relay. It generates all the lab traffic.

```
  node_a (this project) ~~~ relay (EKH05, repeater) ~~~ node_b (root AP)
  192.168.50.10             192.168.50.2                192.168.50.1
                            AP BSSID 02:48:4C:00:00:02
```

When it boots, node A:

1. Loads the AU channel list (802.11 REVmf) and gives the HaLow netif the static address
   `192.168.50.10/24`. It has no DHCP and no gateway.
2. Joins `halow-relay-lab` with WPA3-SAE, PMF required, and an explicit `passphrase_len`
   (issue #11). By default it pins `mmwlan_sta_args.bssid` to the relay AP `02:48:4C:00:00:02`.
   The root and the relay advertise the same SSID, so without the pin A could associate with B
   directly.
3. After the link has been up for 1 s, starts:
   - a UDP echo client to B:5000 (flow `udp`), at 10 datagrams/s with 64-byte payloads;
   - a TCP message stream client to B:5001 (flow `tcp`), at 5 messages/s with 256-byte payloads.
     It reconnects with exponential backoff and jitter;
   - ICMP pings to B (`node_b`) and to the relay (`relay`) every second. Each target has its
     own task and an lwIP raw socket connected to that target (`main/ping_probe.c`), so the two
     probes never see each other's replies;
   - optionally, a second UDP echo client to the relay (flow `udp_relay`).
4. On link down, stops all services, and starts them again once the link is back. Reconnecting is
   done by morselib. Its scan backoff is capped at 10 s here, instead of the SDK's 512 s.

## Roles and baselines

| Kconfig | What it tests |
|---|---|
| `A_MODE_PLAIN_STA` (default) | An ordinary STA with `mmhalow_set_config(WIFI_IF_STA)` + `mmhalow_connect()`. Its link is up whenever it is associated with the relay AP, even if the relay has lost its uplink. The relay brings its AP down after `recovery_timeout_ms` (10 s by default). |
| `A_MODE_RELAY_STA` | A relay-aware STA. Uses `mmhalow_relay_enable()` with only `relay_sta_args` (no relay AP), and selects `CONFIG_HALOW_AP_MODE` and `CONFIG_HALOW_RELAY_MODE`. esp-halow reports the link up only while the AP it joined has a path to the root, and the relay depth is printed (2 behind the relay). |
| `LAB_A_PIN_RELAY` (default) | Associate only with the relay AP BSSID. |
| `LAB_A_PIN_ROOT` | Direct baseline: associate only with node B. Set `CONFIG_LAB_ROOT_AP_BSSID` to B's BSSID, which is in B's `ev=root_up` line, or fix it with the same option on B. |
| `LAB_A_PIN_NONE` | Any AP with the SSID. |
| `LAB_A_STOP_SERVICES_ON_LINK_DOWN=n` | Start services at the first link up and keep them running across drops. Loss counters and the TCP client's `disconnected`/`connected` (`outage_ms`) events then cover the whole outage. |

## Prerequisites

- ESP-IDF v6.0.2: `source ~/.espressif/tools/activate_idf_v6.0.2.sh`.
- The esp-halow submodule at `third_party/esp-halow` (branch `sdk-2.14.3-idf6`). The relay-STA
  role needs its `CONFIG_HALOW_RELAY_MODE` and `mmhalow_relay_enable()`.
- The mm-iot-sdk patches: run `tools/apply_sdk_patches.sh` from the repository root, and again
  after every `git submodule update`. Node A always sets `passphrase_len` itself, so 0003 (#11)
  matters only on the relay. But the relay's AP copies the relay STA's passphrase length, so an
  unpatched relay can lock A out.

## Build, flash, monitor

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
cd node_a
idf.py build                       # target esp32s3 comes from sdkconfig.defaults
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

To build the relay-STA role, set `CONFIG_A_MODE_RELAY_STA=y` in `idf.py menuconfig` (**HaLow lab:
node A (station) → Node A role**). `sdkconfig.defaults` is only read when `sdkconfig` is created,
so delete `sdkconfig` after you edit it.

`sdkconfig.defaults` contains:

- The XIAO ESP32-S3 + WM6108 pins and BCF `bcf_fgh100mhaamd.bin`, the same as esp-halow's
  `configs/sdkconfig.defaults.seeed_xiao_esp32s3-seeed_xiao_mm6108`.
- `CONFIG_HALOW_COUNTRY_CODE="AU"` and `CONFIG_WIFI_MORSE_AU_CHANNELISATION_REVMF=y`.
- `CONFIG_FREERTOS_HZ=1000`, the lwIP options from the esp-halow softap example, a 2 MB factory
  partition, and the console on USB Serial/JTAG (the XIAO's USB-C port).

The build stops with an error if the AU channelisation is not REVmf. At runtime, the node refuses
to start the radio if `CONFIG_LAB_COUNTRY` differs from `CONFIG_HALOW_COUNTRY_CODE`.

## Configuration

**HaLow lab: network plan** holds the same options and defaults as node B: SSID, passphrase,
country, IP plan, ports, report interval, plus:

| Option | Default | Notes |
|---|---|---|
| `CONFIG_LAB_RELAY_AP_BSSID` | `02:48:4C:00:00:02` | Pinned with `LAB_A_PIN_RELAY`. |
| `CONFIG_LAB_ROOT_AP_BSSID` | empty | Used for `LAB_A_PIN_ROOT`, and to label a connection to B as `peer=root`. |
| `CONFIG_LAB_S1G_CHAN_NUM` / `CONFIG_LAB_OP_CLASS` | 37 / 51 | Informational. A warns if it joins another channel. |

**HaLow lab: node A (station)**:

| Option | Default |
|---|---|
| `A_MODE_PLAIN_STA` / `A_MODE_RELAY_STA` | plain |
| `LAB_A_PIN_RELAY` / `_ROOT` / `_NONE` | relay |
| `CONFIG_LAB_A_SCAN_INTERVAL_BASE_S` / `_LIMIT_S` | 2 / 10 |
| `CONFIG_LAB_A_STOP_SERVICES_ON_LINK_DOWN` | y |
| `CONFIG_LAB_A_SERVICE_START_DELAY_MS` | 1000 |
| `CONFIG_LAB_A_HEALTH_INTERVAL_MS` | 10000 |
| `CONFIG_LAB_A_UDP_ECHO_NODE_B` / `CONFIG_LAB_A_UDP_ECHO_RELAY` | y / n (needs a UDP echo server on the relay) |
| `CONFIG_LAB_UDP_RATE_PPS` / `CONFIG_LAB_UDP_PAYLOAD_LEN` | 10 / 64 |
| `CONFIG_LAB_A_TCP_STREAM`, `CONFIG_LAB_TCP_RATE_MPS` / `CONFIG_LAB_TCP_PAYLOAD_LEN` | y, 5 / 256 |
| `CONFIG_LAB_A_TCP_ACK_TIMEOUT_MS` / `_CONNECT_TIMEOUT_MS` / `_BACKOFF_MIN_MS` / `_BACKOFF_MAX_MS` / `_MAX_INFLIGHT` | 5000 / 5000 / 500 / 30000 / 32 |
| `CONFIG_LAB_A_PING_NODE_B` / `CONFIG_LAB_A_PING_RELAY` | y / y |
| `CONFIG_LAB_PING_INTERVAL_MS` / `CONFIG_LAB_A_PING_TIMEOUT_MS` / `CONFIG_LAB_A_PING_SIZE` | 1000 / 1000 / 56 |
| `CONFIG_LAB_A_PING_VERBOSE` | y (a line per reply/timeout; n = summaries only) |
| `CONFIG_LAB_A_STATIC_ARP_NODE_B_MAC` / `_RELAY_MAC` | empty (use ARP) |

### Pinging the relay, and static ARP

The relay's IP stack is bound to its STA interface, so it answers from its **device (STA) MAC**,
not from the AP BSSID. morselib sends a repeater's own broadcasts, including its ARP requests to A,
only upstream, and the root floods them back down. While the relay has no uplink to B, the relay
and A therefore cannot resolve each other, and pings to the relay fail even though A is
associated. Set `CONFIG_LAB_A_STATIC_ARP_RELAY_MAC` to the relay's STA MAC to remove that
dependency on A's side; the relay then also needs a static entry for A. Static entries are
re-installed on every link up, because lwIP clears the ARP table when the netif goes down.

## Output

The traffic services print STATS and EVENT lines (flows `udp`, `udp_relay`, `tcp`), which are
specified in [common/README.md](../common/README.md). The node adds the EVENT and PING lines below,
where `t_ms` is milliseconds since boot. Parse lines with `^(STATS|EVENT|PING),` anywhere in a line.

```
EVENT,node,t_ms=790,ev=boot,app=node_a,role=plain_sta,mac=xx:xx:xx:xx:xx:xx,ip=192.168.50.10,netmask=255.255.255.0,country=AU,pin=relay,bssid_pin=02:48:4C:00:00:02
EVENT,wlan,t_ms=801,ev=sta_start,mode=plain_sta,ssid=halow-relay-lab,pin=relay,err=0
EVENT,wlan,t_ms=805,ev=sta_state,state=connecting,bssid=none
EVENT,wlan,t_ms=4210,ev=sta_state,state=connected,bssid=02:48:4c:00:00:02
EVENT,wlan,t_ms=4300,ev=link_up,bssid=02:48:4c:00:00:02,peer=relay,via_relay=1,pin=relay,rssi=-48,chan=37,op_class=51,depth=na,down_ms=4300
EVENT,svc,t_ms=5300,ev=start,udp=1,udp_relay=0,tcp=1,ping_node_b=1,ping_relay=1
EVENT,udp,t_ms=0,ev=start,dst=192.168.50.1:5000,rate_pps=10,payload=64,count=0,duration_ms=0,session=...
EVENT,tcp,t_ms=2,ev=connected,attempt=1,connect_ms=35,outage_ms=0,next_seq=0
PING,node_b,t_ms=5410,ev=reply,dst=192.168.50.1,seq=1,rtt_ms=21,ttl=64,size=56
PING,relay,t_ms=5395,ev=reply,dst=192.168.50.2,seq=1,rtt_ms=9,ttl=255,size=56
STATS,udp,t_ms=5000,sent=50,rcvd=50,lost=0,dup=0,reord=0,stale=0,loss_ppm=0,rtt_n=50,rtt_min_us=...,hist=...,tx_err=0,bad=0,foreign=0,corrupt=0,final=0
PING,node_b,t_ms=10300,ev=summary,dst=192.168.50.1,sent=5,rcvd=5,lost=0,loss_ppm=0,rtt_min_ms=18,rtt_avg_ms=22,rtt_max_ms=31,running=1
EVENT,node,t_ms=14300,ev=health,uptime_s=14,heap_free=...,heap_min=...,role=plain_sta,link=1,rssi=-48,svc=running,evt_drops=0
EVENT,wlan,t_ms=60000,ev=link_down,up_ms=55700
EVENT,svc,t_ms=60000,ev=stop,reason=link_down
EVENT,svc,t_ms=60150,ev=stopped,wait_ms=150
```

In the default plain-STA build, `link_up` reports `chan=0,op_class=0`. Reading the channel needs
`mmwlan_get_vif_channel_info()`, which only links when `CONFIG_HALOW_AP_MODE` is set (the relay-STA
variant sets it), because esp-halow compiles morselib's AP code only then. The example above shows
the relay-STA variant.

| Line | Meaning |
|---|---|
| `ev=sta_start` / `sta_start_failed` | Result of `mmhalow_connect()` or `mmhalow_relay_enable()`. On failure it is retried every 5 s. |
| `ev=sta_state` | morselib STA state callback: `connecting`, `connected` (with BSSID) or `disabled`. |
| `ev=link_up` | The netif link came up. `bssid` is the AP that A joined. `peer` is `relay` if it equals `CONFIG_LAB_RELAY_AP_BSSID`, `root` if it equals `CONFIG_LAB_ROOT_AP_BSSID`, otherwise `other`. `via_relay=1` means A is on the relay. `depth` is the relay depth (relay-STA role only). `down_ms` is how long the link was down. |
| `ev=link_down` | `up_ms` is how long the link was up. |
| `EVENT,relay,ev=status` | Relay status callback in the relay-STA role: `state`, `sta_state`, `depth`. |
| `EVENT,svc,ev=start/stop/stopped/exit` | Service lifecycle. `exit` means a service returned an error and is retried. |
| `EVENT,arp,ev=static` / `static_fail` | Static ARP entries, if configured. |
| `PING,<target>,ev=reply/timeout` | One ICMP echo (with `CONFIG_LAB_A_PING_VERBOSE`). `seq` restarts at 1 whenever pings restart. `rtt_ms` has 1 ms resolution. |
| `PING,<target>,ev=summary` | Counters accumulated since boot, printed every report interval and when pings stop (`running=0`). `lost = sent - rcvd`. |
| `ev=health` | Uptime, heap, link, RSSI and service state. |
| `EVENT,node,ev=fatal` | Startup failed (`country_mismatch`, `static_ip_failed`, `bad_relay_bssid`, `bad_root_bssid`). The node halts. |

Comparing `PING,relay` with `PING,node_b` separates the A <-> relay hop from the relay <-> B hop.
