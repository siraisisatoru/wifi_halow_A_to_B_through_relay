<!--
Copyright 2026 siraisisatoru
SPDX-License-Identifier: Apache-2.0
-->

# common: lab protocol and traffic services

Portable C11 code shared by node A (ESP32-S3 STA), node B (ESP32-S3 root AP), the EKH05 relay
(optional) and the host simulation:

| Piece | Files | Purpose |
|---|---|---|
| Network plan | `include/halow_lab_config.h` | Canonical defaults (SSID, channel, BSSID, IPs, ports, rates), all overridable |
| Wire format | `include/linkproto.h`, `src/linkproto.c` | `HLRL` packets, explicit big-endian encode/decode, strict validation, CRC-32 |
| Statistics | `include/linkstats.h`, `src/linkstats.c` | Loss/dup/reorder (1024-seq window), RTT min/avg/max, RFC 3550 jitter, histogram, `STATS` line |
| TCP framing | `include/msgstream.h`, `src/msgstream.c` | Length-prefixed frames, incremental parser, resync on corruption |
| Platform | `include/lab_platform.h`, `port/{posix,esp_idf,mmosal}/` | Time, sleep, console lines, logging, random, malloc, tasks |
| Services | `include/lab_services.h`, `src/svc_udp_echo.c`, `src/svc_tcp_stream.c` | UDP echo client/server, TCP stream client/server |
| Host tool | `host/lab_host.c` | CLI around the services (netns simulation, loopback test) |
| Tests | `test/` | Unit tests (plain C runner) and `test_loopback.sh` |

Nothing allocates per packet. Each `*_run()` call allocates its working context once with
`lab_malloc()`: about 3 KiB for the UDP services and 4 KiB for the TCP services. Run each service
in its own task with a 6 KiB stack. Call `lab_platform_init()` once first.

## Using it

### ESP-IDF (nodes A and B)

The directory is an ESP-IDF component named `common`:

```cmake
# project CMakeLists.txt
set(EXTRA_COMPONENT_DIRS ../../common)
```

```cmake
# main/CMakeLists.txt
idf_component_register(SRCS ... REQUIRES common)
```

The component builds without warnings with ESP-IDF v6.0.2 for esp32s3. `halow_lab_config.h`
includes `sdkconfig.h` and picks up any of these Kconfig options when an app defines them:
`CONFIG_LAB_SSID`, `CONFIG_LAB_PASSPHRASE`, `CONFIG_LAB_COUNTRY`, `CONFIG_LAB_S1G_CHAN_NUM`,
`CONFIG_LAB_OP_CLASS`, `CONFIG_LAB_PRI_BW_MHZ`, `CONFIG_LAB_PRI_1MHZ_CHAN_IDX`,
`CONFIG_LAB_RELAY_AP_BSSID` (string), `CONFIG_LAB_ROOT_AP_BSSID` (string), `CONFIG_LAB_IP_NODE_B`,
`CONFIG_LAB_IP_RELAY`, `CONFIG_LAB_IP_NODE_A`, `CONFIG_LAB_NETMASK`, `CONFIG_LAB_GATEWAY`,
`CONFIG_LAB_UDP_ECHO_PORT`, `CONFIG_LAB_TCP_STREAM_PORT`, `CONFIG_LAB_UDP_RATE_PPS`,
`CONFIG_LAB_UDP_PAYLOAD_LEN`, `CONFIG_LAB_TCP_RATE_MPS`, `CONFIG_LAB_TCP_PAYLOAD_LEN`,
`CONFIG_LAB_REPORT_INTERVAL_MS`, `CONFIG_LAB_PING_INTERVAL_MS`.

### mm-iot-sdk make flow (EKH05 relay)

```make
# after the SDK mk/*.mk includes, before the "$(ELF_FILE): $(OBJS)" rule
LAB_COMMON_DIR := $(abspath $(APP_DIR)/../common)
include $(LAB_COMMON_DIR)/mk/common.mk
```

This appends the objects to `OBJS`, adds `include/` to `CFLAGS` and compiles into
`$(BUILD_DIR)/lab_common/` with the app's own `CFLAGS`/`CONLYFLAGS`, so the SDK's
`-Werror -Wall -Wextra -Wc++-compat -std=gnu11` apply. It needs the SDK lwIP socket API, which is
on by default (`LWIP_SOCKET` defaults to 1, `lwipopts.h` sets `LWIP_SO_RCVTIMEO=1` and
`SO_REUSE=1`). `errno` is the newlib global that lwIP's `sockets.c` also uses. Things to know on
this target:

- `lab_time_us()` has 1 ms resolution because it extends `mmosal_get_time_ms()` to 64 bits.
- newlib-nano has no `%llu`, so no 64-bit `printf` conversions are used.
- `lab_out_line()` uses `printf`.

Checked with arm-none-eabi-gcc 14.2.1 (ekh05-builder) using the relay_mode example's flags.
The library adds about 13.6 KiB of text.

### Host (Docker, nothing installed on the host)

```sh
docker build -t halow-hostsim:1 docker/hostsim
docker run --rm -v "$PWD":/work halow-hostsim:1 sh -c \
  'cmake -S common -B /tmp/b -DLAB_SANITIZE=ON && cmake --build /tmp/b -j && \
   ctest --test-dir /tmp/b --output-on-failure'
```

`LAB_SANITIZE=ON` builds with `-fsanitize=address,undefined`. `LAB_WERROR` defaults to ON. The
outputs are `lab_unit_tests`, `lab_host` and `libhalow_lab_common.a`.

```sh
lab_host udp-server [--port 5000] [--report-ms N]
lab_host udp-client --dst 192.168.50.1 [--rate 10] [--size 64] [--count N] [--duration-ms N] [--drain-ms N]
lab_host tcp-server [--port 5001] [--idle-ms N]
lab_host tcp-client --dst 192.168.50.1 [--rate 5] [--size 256] [--count N] [--ack-timeout-ms N]
                    [--backoff-min-ms N] [--backoff-max-ms N] [--inflight N]
# all: --flow NAME --report-ms N; clients: --max-lost N  (exit 2 if exceeded)
```

## Network plan defaults (`halow_lab_config.h`)

| Macro | Default | Notes |
|---|---|---|
| `LAB_SSID` | `halow-relay-lab` | root AP and relay AP |
| `LAB_PASSPHRASE` | `change-me-halow-au` | placeholder, must be changed |
| `LAB_SECURITY_SAE` / `LAB_PMF_REQUIRED` | 1 / 1 | WPA3-SAE, PMF required |
| `LAB_COUNTRY` | `AU` | all nodes use the 802.11 REVmf AU list |
| `LAB_S1G_CHAN_NUM` / `LAB_OP_CLASS` / `LAB_S1G_OP_CLASS` | 37 / 51 / 23 | 2 MHz at 920.5 MHz (`s1g_channels_AU[23..]` entry) |
| `LAB_CHAN_BW_MHZ` / `LAB_PRI_BW_MHZ` / `LAB_PRI_1MHZ_CHAN_IDX` | 2 / 2 / 0 | |
| `LAB_RELAY_AP_BSSID_STR` / `_INIT` | `02:48:4C:00:00:02` | relay AP BSSID; node A pins it |
| `LAB_ROOT_AP_BSSID_STR` / `_INIT` | empty / zeros | relay STA: any root |
| `LAB_IP_NODE_B` / `LAB_IP_RELAY` / `LAB_IP_NODE_A` | 192.168.50.1 / .2 / .10 | /24, no gateway, no DHCP |
| `LAB_UDP_ECHO_PORT` / `LAB_TCP_STREAM_PORT` | 5000 / 5001 | |
| `LAB_UDP_RATE_PPS` / `LAB_UDP_PAYLOAD_LEN` / `LAB_UDP_DRAIN_MS` | 10 / 64 / 2000 | |
| `LAB_TCP_RATE_MPS` / `LAB_TCP_PAYLOAD_LEN` / `LAB_TCP_MAX_INFLIGHT` | 5 / 256 / 32 | |
| `LAB_TCP_ACK_TIMEOUT_MS` / `LAB_TCP_CONNECT_TIMEOUT_MS` | 5000 / 5000 | |
| `LAB_TCP_BACKOFF_MIN_MS` / `LAB_TCP_BACKOFF_MAX_MS` | 500 / 30000 | |
| `LAB_TCP_SERVER_IDLE_TIMEOUT_MS` | 60000 | |
| `LAB_REPORT_INTERVAL_MS` / `LAB_PING_INTERVAL_MS` / `LAB_PING_SIZE` | 5000 / 1000 / 56 | |

Node B only answers traffic and never starts any. Its ARP entry for A is learned from A's ARP
request, so B never has to send a broadcast. With esp-halow `3c46c92` B's own group-addressed
frames work anyway (its TX glue gives them an RA, which sidesteps #10; patch 0002 is the SDK-side
fix), but this keeps the design independent of that workaround.

## Wire format (`linkproto`)

All fields are big-endian. The header is 32 bytes and is followed by `payload_len` bytes.

| Off | Size | Field | Rules |
|---|---|---|---|
| 0 | 4 | magic | `0x484C524C` ("HLRL") |
| 4 | 1 | version | 1 |
| 5 | 1 | type | 1 ECHO_REQ, 2 ECHO_RSP, 3 MSG, 4 MSG_ACK |
| 6 | 1 | flags | bit0 `LAST` (final packet of a run); other bits must be 0 |
| 7 | 1 | reserved | 0 |
| 8 | 4 | seq | starts at 0 per session, wraps at 2^32 |
| 12 | 4 | session | random per client run, echoed |
| 16 | 8 | ts_us | sender's monotonic clock at send, echoed unchanged |
| 24 | 2 | status | 0 in requests; responses: 0 OK, 1 BAD_PAYLOAD |
| 26 | 2 | payload_len | at most 1400 (`LINKPROTO_MAX_PAYLOAD`) |
| 28 | 4 | crc32 | IEEE/zlib CRC-32 over bytes 0..27 then the payload (`zlib.crc32` in Python) |

`linkproto_decode()` requires the exact length and checks magic, version, type, flags, the
reserved byte, the status in request types, the length limit, header plus payload length equal
to the buffer length, and the CRC. Payloads carry a deterministic pattern
(`linkproto_fill_pattern(seq)`) that the receivers check.

Echo: B returns the request as `ECHO_RSP` with the same seq, session, ts_us, flags and payload.
RTT is A's receive time minus `ts_us`, all on A's clock.

## TCP framing (`msgstream`)

Each frame is a `u16` big-endian length L followed by L bytes of one linkproto packet, with
32 <= L <= 32 + 1024 (`MSGSTREAM_MAX_PAYLOAD`). The parser takes any chunking. When it meets an
out-of-range length, a bad header or a bad CRC, it drops bytes until a length prefix is followed
by the `HLRL` magic again. A corrupted frame costs only that frame, and it is counted in
`resyncs`/`bytes_discarded`/`*_errors`. A dropped connection does not count as corruption:
`msgstream_parser_reset()` discards the partial frame.

The client sends `MSG` and the server answers each one with an empty `MSG_ACK` carrying the same
seq, session and ts_us. The ACK status is `BAD_PAYLOAD` if the pattern did not match.

## Console line formats

Services print machine-readable lines with `lab_out_line()`, one per line with no prefix. On
ESP-IDF they come from `printf` and so may be mixed with `I (1234) tag: ...` log lines; parsers
should match `^(STATS|EVENT),` anywhere in a line. Fields are separated by `,`, and every field
after the flow name is `key=value`. Keys appear in a fixed order, but parsers must ignore unknown
keys because more may be appended later. In every line `t_ms` is milliseconds since that service's
`*_run()` call started.

### STATS

```
STATS,<flow>,t_ms=,sent=,rcvd=,lost=,dup=,reord=,stale=,loss_ppm=,rtt_n=,rtt_min_us=,rtt_avg_us=,rtt_max_us=,jitter_us=,hist=c0/c1/.../c11<extras>
```

The counters are cumulative for the session; to get per-interval rates, diff consecutive lines.

| Key | Meaning |
|---|---|
| sent | packets sent by this side (client: requests/messages; server: echoes/ACKs) |
| rcvd | unique sequence numbers received (client: echoes/ACKs; server: requests/messages) |
| lost | sequence numbers skipped below the highest seen, minus late arrivals. The final line (`final=1`) also counts the tail after the highest seen |
| dup | already-seen sequence numbers within the 1024-seq window |
| reord | arrivals below the highest seen that fill a gap (each one also decrements `lost`) |
| stale | arrivals older than the window or older than the flow start, otherwise ignored |
| loss_ppm | lost / (rcvd + lost) x 10^6 |
| rtt_* | client only: RTT samples (non-duplicate replies). `rtt_avg_us` is the integer mean; 0 when `rtt_n=0` |
| jitter_us | RFC 3550 A.8 estimator. Client: on consecutive RTTs in arrival order. Server: on one-way transit (receive time minus `ts_us`, clock offset cancels) |
| hist | 12 RTT bucket counts. Bucket i holds `rtt < edge[i]` with edges 1, 2, 5, 10, 20, 50, 100, 200, 500 ms, 1 s, 2 s; the last bucket holds `>= 2 s` |

Extras by flow (default flow names; the `flow` config field can change them):

| Flow | Extras |
|---|---|
| `udp` (A, echo client) | `tx_err` (sendto failures, still counted as sent and so as lost), `bad` (undecodable, wrong type, unsent seq, future ts), `foreign` (other source or session), `corrupt` (pattern mismatch), `final` |
| `udp_srv` (B) | `peer=ip:port`, `session` (hex), `bad`, `tx_err`, `final` |
| `tcp` (A, stream client) | `up` (0/1), `connects`, `disconnects`, `stalls`, `connect_fail`, `inflight`, `bad_status` (ACKs reporting BAD_PAYLOAD), `proto_err`, `crc_err`, `resyncs`, `final` |
| `tcp_srv` (B) | `session`, `up`, `accepts`, `bad_type`, `bad_payload`, `crc_err`, `hdr_err`, `len_err`, `resyncs`, `discarded`, `ack_drop`, `final` |

What the loss numbers mean in each direction:

- **UDP:** the client's `lost` is round-trip loss. The server's `lost` is uplink loss (A to B)
  for that session, so downlink loss is roughly client lost minus server lost.
- **TCP client:** `lost` counts messages that were never ACKed. When a connection is dropped, the
  outstanding messages are given up and seq numbering continues after reconnect, so they appear
  as a gap.
- **TCP server:** `lost` counts messages that never arrived.

Servers report only while a session is active. A session ends on `LAST`, when a new session
starts, or on stop, and each end prints a `final=1` line.

### EVENT

```
EVENT,<flow>,t_ms=<ms>,ev=<name>[,key=value...]
```

| Flow | ev | Keys |
|---|---|---|
| udp, tcp | `start` | `dst=ip:port`, `rate_pps`/`rate_mps`, `payload`, `count`, `duration_ms`, `session` |
| udp | `end` | `reason=done\|stopped`, `sent` |
| tcp | `connecting` | `attempt` (1-based, consecutive attempts without a successful ACK) |
| tcp | `connected` | `attempt`, `connect_ms`, `outage_ms` (since the last disconnect; 0 for the first), `next_seq` |
| tcp | `connect_failed` | `attempt`, `err` (errno), `retry_ms` |
| tcp | `disconnected` | `reason=eof\|recv_error\|send_error\|stall`, `err`, `unacked`, `oldest_age_ms`, `up_ms`, `retry_ms` |
| tcp | `end` | `reason=done\|stopped\|ack_timeout\|disconnected`, `sent`, `unacked`, `abandoned` |
| udp_srv, tcp_srv | `listening` | `proto`, `port` |
| udp_srv | `session` | `peer`, `session`, `first_seq` |
| tcp_srv | `session` | `session`, `first_seq` |
| udp_srv, tcp_srv | `session_end` | `session`, `reason=last\|new_session\|stopped` |
| udp_srv | `post_end` | `session`, `seq`, `count`, `echoed`: a request for the session the client last ended with its LAST request (a late or duplicated datagram). It is echoed but not counted, and does not start a new session. |
| tcp_srv | `accepted` | `peer`, `accepts` |
| tcp_srv | `closed` | `reason=eof\|recv_error\|send_error\|idle\|replaced\|stopped`, `err`, `up_ms` |
| all | `stopped` | (servers) |

TCP client behaviour:

- **Stall:** the oldest unACKed message is older than `ack_timeout_ms`.
- **Reconnect delay:** `min * 2^(attempt-1)` capped at `max`, then drawn uniformly from
  `[d/2, d]`. The attempt count resets when an ACK arrives.
- **Window:** at most `max_inflight` messages are outstanding.
- **End of run:** after the count or duration is reached the client waits up to
  `ack_timeout_ms` for the last ACKs. If a stall, EOF or error drops the connection during that
  wait, `ev=end` reports `reason=disconnected`, and its `unacked` counts the messages abandoned
  then. `abandoned` is the run total of messages lost to disconnects plus those still in flight
  at the end, for cross-checking against the final STATS `lost`.

The TCP server always adopts the newest connection, so a client that reconnects after a relay
drop is not stuck behind B's half-open old connection.

## Tests

`test/` contains:

- **linkproto:** CRC vectors; round trips for every type over many sizes; byte layout; in-place
  and overlapping encode; every encode and decode validation error; every single-bit flip rejected.
- **linkstats:** loss, late correction, duplicates, window edge and stale handling, wrap-around at
  2^32, finalize, RTT and histogram edges, RFC 3550 jitter (exact first step and a float
  reference), formatting and truncation, and a randomised check that rcvd + lost adds up.
- **msgstream:** whole-stream, 1-byte chunks, random splits, every byte of a frame corrupted
  (exactly that frame lost, one resync), garbage between frames, truncation and reset, length
  limits.
- **`test_loopback.sh`:**
  - UDP and TCP with no loss
  - TCP client started before the server (backoff)
  - TCP server restarted mid-run (reconnect)
