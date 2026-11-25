#!/usr/bin/env bash
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Loopback integration test of the traffic services through the lab_host tool:
#   1. UDP echo, lossless.
#   2. TCP stream, lossless.
#   3. TCP client started before the server: connect failures, backoff, then success.
#   4. TCP server killed and restarted mid-run: client reconnects and finishes.
#
# usage: test_loopback.sh <path to lab_host>

set -u
HOST_BIN=${1:?usage: test_loopback.sh <lab_host>}
WORK=$(mktemp -d)
UDP_PORT=$((20000 + RANDOM % 10000))
TCP_PORT=$((UDP_PORT + 1))
PIDS=()
FAILED=0

cleanup()
{
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

fail()
{
    echo "FAIL: $*"
    FAILED=1
}

expect_line()
{
    local file=$1 pattern=$2
    grep -Eq "$pattern" "$file" || fail "$file: no line matching '$pattern'"
}

echo "== UDP echo"
"$HOST_BIN" udp-server --port "$UDP_PORT" --report-ms 500 >"$WORK/udp_srv.log" 2>&1 &
PIDS+=($!)
sleep 0.3
if ! "$HOST_BIN" udp-client --dst 127.0.0.1 --port "$UDP_PORT" --rate 200 --size 200 \
        --count 400 --report-ms 500 --drain-ms 500 --max-lost 0 >"$WORK/udp_cli.log" 2>&1; then
    fail "udp-client exit status"
fi
expect_line "$WORK/udp_cli.log" '^STATS,udp,.*,sent=400,rcvd=400,lost=0,dup=0,.*,final=1$'
expect_line "$WORK/udp_srv.log" '^EVENT,udp_srv,.*ev=session,'
expect_line "$WORK/udp_srv.log" '^STATS,udp_srv,.*,rcvd=400,lost=0,.*,final=1$'
expect_line "$WORK/udp_srv.log" 'ev=session_end,.*reason=last'

echo "== TCP stream"
"$HOST_BIN" tcp-server --port "$TCP_PORT" --report-ms 500 >"$WORK/tcp_srv.log" 2>&1 &
TCP_SRV=$!
PIDS+=($TCP_SRV)
sleep 0.3
if ! "$HOST_BIN" tcp-client --dst 127.0.0.1 --port "$TCP_PORT" --rate 200 --size 300 \
        --count 400 --report-ms 500 --max-lost 0 >"$WORK/tcp_cli.log" 2>&1; then
    fail "tcp-client exit status"
fi
expect_line "$WORK/tcp_cli.log" '^EVENT,tcp,.*ev=connected,attempt=1,'
expect_line "$WORK/tcp_cli.log" '^STATS,tcp,.*,sent=400,rcvd=400,lost=0,dup=0,.*,final=1$'
expect_line "$WORK/tcp_srv.log" '^STATS,tcp_srv,.*,rcvd=400,lost=0,.*bad_payload=0,crc_err=0,.*final=1$'
kill "$TCP_SRV" 2>/dev/null
wait "$TCP_SRV" 2>/dev/null

echo "== TCP client before server (backoff)"
TCP_PORT2=$((TCP_PORT + 1))
"$HOST_BIN" tcp-client --dst 127.0.0.1 --port "$TCP_PORT2" --rate 50 --count 100 \
    --backoff-min-ms 100 --backoff-max-ms 400 --report-ms 0 --max-lost 0 \
    >"$WORK/tcp_cli2.log" 2>&1 &
CLI2=$!
sleep 1.2
"$HOST_BIN" tcp-server --port "$TCP_PORT2" --report-ms 0 >"$WORK/tcp_srv2.log" 2>&1 &
SRV2=$!
PIDS+=($SRV2)
if ! wait "$CLI2"; then
    fail "tcp-client (backoff) exit status"
fi
expect_line "$WORK/tcp_cli2.log" 'ev=connect_failed,attempt=1,'
expect_line "$WORK/tcp_cli2.log" 'ev=connect_failed,attempt=3,'
expect_line "$WORK/tcp_cli2.log" 'ev=connected,attempt=[0-9]+,'
expect_line "$WORK/tcp_cli2.log" '^STATS,tcp,.*,sent=100,rcvd=100,lost=0,.*,final=1$'
kill "$SRV2" 2>/dev/null
wait "$SRV2" 2>/dev/null

echo "== TCP server restart mid-run (reconnect)"
TCP_PORT3=$((TCP_PORT + 2))
"$HOST_BIN" tcp-server --port "$TCP_PORT3" --report-ms 0 >"$WORK/tcp_srv3a.log" 2>&1 &
SRV3=$!
PIDS+=($SRV3)
sleep 0.3
"$HOST_BIN" tcp-client --dst 127.0.0.1 --port "$TCP_PORT3" --rate 100 --count 300 \
    --backoff-min-ms 100 --backoff-max-ms 400 --report-ms 0 --max-lost 40 \
    >"$WORK/tcp_cli3.log" 2>&1 &
CLI3=$!
sleep 1.0
kill "$SRV3"
wait "$SRV3" 2>/dev/null
sleep 0.5
"$HOST_BIN" tcp-server --port "$TCP_PORT3" --report-ms 0 >"$WORK/tcp_srv3b.log" 2>&1 &
SRV3B=$!
PIDS+=($SRV3B)
if ! wait "$CLI3"; then
    fail "tcp-client (reconnect) exit status"
fi
expect_line "$WORK/tcp_cli3.log" 'ev=disconnected,reason=(eof|recv_error|send_error),'
expect_line "$WORK/tcp_cli3.log" 'ev=connected,.*outage_ms=[0-9]+'
expect_line "$WORK/tcp_cli3.log" '^STATS,tcp,.*,sent=300,.*disconnects=1,.*,final=1$'
expect_line "$WORK/tcp_srv3b.log" 'ev=session,.*first_seq=[1-9][0-9]*'

if [ "$FAILED" -ne 0 ]; then
    for f in "$WORK"/*.log; do
        echo "---- $f"
        cat "$f"
    done
    exit 1
fi
echo "loopback: all passed"
