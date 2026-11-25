#!/usr/bin/env python3
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
"""Parse lab STATS/EVENT/ping lines from simulation logs or real serial logs into CSV + JSON.

Works on anything that contains the common/ line formats (see common/README.md), anywhere in a
line, so ESP-IDF monitor output with interleaved ESP_LOG lines is fine. Standard library only.

Recognised lines
  STATS,<flow>,t_ms=...,key=value...       common/ services (cumulative counters)
  EVENT,<flow>,t_ms=...,ev=<name>,...      common/ services, apps and netsim
  PING,<flow>,t_ms=..,ev=reply|timeout|summary,...   node A firmware (node_a/main/ping_probe.c):
      reply:   dst=..,seq=..,rtt_ms=..,ttl=..,size=..
      timeout: dst=..,seq=..
      summary: dst=..,sent=..,rcvd=..,rtt_min_ms=..,rtt_avg_ms=..,rtt_max_ms=.. (cumulative)
  PING,<flow>,t_ms=..,dst=..,seq=..,rtt_us=..[,ttl=..] | status=timeout   (generic machine format)
  Machine-format flows are reported as ping_<flow> (node A's "node_b" becomes "ping_node_b").
  iputils:  "64 bytes from 192.168.50.1: icmp_seq=3 ttl=64 time=12.3 ms", "no answer yet for icmp_seq=4"
  esp_ping: "64 bytes from 192.168.50.1 icmp_seq=3 ttl=64 time=12 ms", "From 192.168.50.1 icmp_seq=4 timeout"

Optional host timestamp prefixes (used as the time axis when present)
  "[  12.345678] "              netsim / any capture tool printing seconds
  "2026-11-03 12:00:00.123 "    idf.py monitor --timestamps, or similar ISO-like prefix
  "[2026-11-03 12:00:00.123] "  same in brackets
Without a prefix each flow's own t_ms is used.

Usage
  parse_logs.py [-o OUTDIR] [--name NAME] LOG [LOG ...]       any logs (e.g. serial captures)
  parse_logs.py --sim-dir sim/results/<scenario>              a netsim result directory
Writes OUTDIR/{stats.csv,events.csv,ping.csv,summary.json} and prints a short table.
"""

import argparse
import csv
import datetime as _dt
import glob
import gzip
import json
import math
import os
import re
import struct
import sys

HIST_EDGES_MS = [0, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000]
NODE_NAMES = {"192.168.50.1": "node_b", "192.168.50.2": "relay", "192.168.50.10": "node_a"}
LINKPROTO_MAGIC = 0x484C524C

RE_PREFIX_NUM = re.compile(r"^\[\s*(-?\d+(?:\.\d+)?)\]\s?")
RE_PREFIX_ISO = re.compile(r"^\[?(\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:\.\d+)?)\]?\s?")
RE_RECORD = re.compile(r"(STATS|EVENT|PING),([A-Za-z0-9_.:-]+),(\S*)")
RE_PING_REPLY = re.compile(
    r"(\d+) bytes from (\d+\.\d+\.\d+\.\d+):? icmp_seq=(\d+) ttl=(\d+) time=([\d.]+) ?ms")
RE_PING_NOANS = re.compile(r"no answer yet for icmp_seq=(\d+)")
RE_PING_TIMEOUT = re.compile(r"From (\d+\.\d+\.\d+\.\d+) icmp_seq=(\d+) timeout")
RE_PING_HEADER = re.compile(r"^PING (\d+\.\d+\.\d+\.\d+) ")
RE_PING_SUMMARY = re.compile(r"(\d+) packets transmitted, (\d+) (?:packets )?received")


def _num(v):
    try:
        return int(v)
    except ValueError:
        try:
            return float(v)
        except ValueError:
            return v


def _kv(fields):
    out = {}
    for f in fields:
        if "=" in f:
            k, v = f.split("=", 1)
            out[k] = _num(v) if k != "session" else v
    return out


def ping_flow(ip):
    return "ping_" + NODE_NAMES.get(ip, ip.replace(".", "_"))


# --------------------------------------------------------------------------------------------
# Line parsing
# --------------------------------------------------------------------------------------------


class Records:
    def __init__(self):
        self.stats = []
        self.events = []
        self.pings = []
        # flow -> {"sent", "rcvd"[, "rtt_min_ms", "rtt_avg_ms", "rtt_max_ms"]}
        self.ping_totals = {}
        self.ping_dst = {}
        self._iso_t0 = None
        self._ping_seq = {}
        self._ping_sum = {}

    def _machine_ping(self, src, lineno, host_t, flow, kv):
        if not flow.startswith("ping_"):
            flow = "ping_" + flow
        if kv.get("dst") is not None:
            self.ping_dst.setdefault(flow, kv.get("dst"))
        ev = kv.get("ev")
        key = (src, flow)
        if ev == "summary":
            self._ping_summary(key, kv)
            return
        seq = kv.get("seq")
        # A recreated probe (node A restarts its services on link-up) may count seq again.
        last = self._ping_seq.get(key)
        run = last[1] if last else 0
        if last is not None and isinstance(seq, int) and isinstance(last[0], int) and seq < last[0]:
            run += 1
        self._ping_seq[key] = (seq, run)
        if "rtt_us" in kv:
            rtt = kv["rtt_us"] / 1000.0
        elif "rtt_ms" in kv:
            rtt = float(kv["rtt_ms"])
        else:
            rtt = None
        if ev == "timeout" or kv.get("status") == "timeout":
            rtt = None
        self.pings.append({"src": src, "line": lineno, "host_t": host_t, "flow": flow,
                           "t_ms": kv.get("t_ms"), "dst": kv.get("dst"), "seq": seq,
                           "rtt_ms": rtt, "ttl": kv.get("ttl"), "run": run})

    def _ping_summary(self, key, kv):
        """Cumulative ev=summary lines; a drop in sent means the probe was recreated."""
        sent, rcvd = kv.get("sent"), kv.get("rcvd")
        if not isinstance(sent, int) or not isinstance(rcvd, int):
            return
        st = self._ping_sum.setdefault(key, {"done": [], "cur": None})
        if st["cur"] is not None and sent < st["cur"]["sent"]:
            st["done"].append(st["cur"])
        st["cur"] = {"sent": sent, "rcvd": rcvd, "min": kv.get("rtt_min_ms"),
                     "avg": kv.get("rtt_avg_ms"), "max": kv.get("rtt_max_ms")}
        runs = st["done"] + [st["cur"]]
        tot = {"sent": sum(x["sent"] for x in runs), "rcvd": sum(x["rcvd"] for x in runs)}
        with_rtt = [x for x in runs if x["rcvd"] and x["min"] is not None]
        if with_rtt:
            tot["rtt_min_ms"] = min(x["min"] for x in with_rtt)
            tot["rtt_max_ms"] = max(x["max"] for x in with_rtt)
            tot["rtt_avg_ms"] = sum(x["avg"] * x["rcvd"] for x in with_rtt) / sum(
                x["rcvd"] for x in with_rtt)
        self.ping_totals[key[1]] = tot

    def _host_time(self, line):
        m = RE_PREFIX_NUM.match(line)
        if m:
            return float(m.group(1)), line[m.end():]
        m = RE_PREFIX_ISO.match(line)
        if m:
            ts = _dt.datetime.fromisoformat(m.group(1).replace(" ", "T")).timestamp()
            if self._iso_t0 is None:
                self._iso_t0 = ts
            return ts - self._iso_t0, line[m.end():]
        return None, line

    def feed_file(self, path, src=None):
        src = src or os.path.splitext(os.path.basename(path))[0]
        opener = gzip.open if path.endswith(".gz") else open
        ping_target = None
        with opener(path, "rt", encoding="utf-8", errors="replace") as f:
            for lineno, raw in enumerate(f, 1):
                line = raw.rstrip("\r\n")
                host_t, body = self._host_time(line)

                m = RE_RECORD.search(body)
                if m:
                    kind, flow, rest = m.group(1), m.group(2), m.group(3)
                    kv = _kv(rest.split(","))
                    rec = {"src": src, "line": lineno, "host_t": host_t, "flow": flow}
                    if kind == "STATS":
                        rec.update(kv)
                        self.stats.append(rec)
                    elif kind == "EVENT":
                        rec.update(kv)
                        self.events.append(rec)
                    else:
                        self._machine_ping(src, lineno, host_t, flow, kv)
                    continue

                m = RE_PING_HEADER.search(body)
                if m:
                    ping_target = m.group(1)
                    continue
                m = RE_PING_REPLY.search(body)
                if m:
                    ip = m.group(2)
                    self.pings.append({"src": src, "line": lineno, "host_t": host_t,
                                       "flow": ping_flow(ip), "t_ms": None, "dst": ip,
                                       "seq": int(m.group(3)), "rtt_ms": float(m.group(5)),
                                       "ttl": int(m.group(4))})
                    continue
                m = RE_PING_TIMEOUT.search(body)
                if m:
                    ip = m.group(1)
                    self.pings.append({"src": src, "line": lineno, "host_t": host_t,
                                       "flow": ping_flow(ip), "t_ms": None, "dst": ip,
                                       "seq": int(m.group(2)), "rtt_ms": None, "ttl": None})
                    continue
                m = RE_PING_NOANS.search(body)
                if m and ping_target:
                    self.pings.append({"src": src, "line": lineno, "host_t": host_t,
                                       "flow": ping_flow(ping_target), "t_ms": None,
                                       "dst": ping_target, "seq": int(m.group(1)),
                                       "rtt_ms": None, "ttl": None})
                    continue
                m = RE_PING_SUMMARY.search(body)
                if m and ping_target:
                    self.ping_totals[ping_flow(ping_target)] = {"sent": int(m.group(1)),
                                                                "rcvd": int(m.group(2))}


# --------------------------------------------------------------------------------------------
# Statistics helpers
# --------------------------------------------------------------------------------------------


def percentile(values, p):
    if not values:
        return None
    v = sorted(values)
    k = (len(v) - 1) * p / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def hist_percentile(hist, p):
    """Percentile from the 12 STATS buckets, linear inside a bucket. The open last bucket
    (>= 2 s) returns 2000 ms."""
    total = sum(hist)
    if total == 0:
        return None
    target = total * p / 100.0
    acc = 0
    for i, c in enumerate(hist):
        if c and acc + c >= target:
            if i == len(hist) - 1:
                return float(HIST_EDGES_MS[-1])
            lo, hi = HIST_EDGES_MS[i], HIST_EDGES_MS[i + 1]
            return lo + (hi - lo) * (target - acc) / c
        acc += c
    return float(HIST_EDGES_MS[-1])


def hist_pct_clamped(hist, p, end):
    """Histogram percentile clamped to the exact min/max of the same STATS line."""
    if not hist or not end.get("rtt_n"):
        return None
    v = hist_percentile(hist, p)
    if v is None:
        return None
    lo, hi = end.get("rtt_min_us", 0) / 1000.0, end.get("rtt_max_us", 0) / 1000.0
    return r(min(max(v, lo), hi))


def r(v, nd=3):
    return None if v is None else round(v, nd)


# --------------------------------------------------------------------------------------------
# Segments: one run of one flow (a client run, or one server session)
# --------------------------------------------------------------------------------------------


def segment_stats(stats):
    """Split STATS rows into segments keyed by (src, flow, session-or-run)."""
    segs = {}
    order = []
    last_t = {}
    run_idx = {}
    for row in stats:
        key0 = (row["src"], row["flow"])
        if "session" in row:
            # Server lines carry the client's session id.
            key = key0 + (str(row["session"]),)
        else:
            prev = last_t.get(key0)
            if prev is not None and (row.get("t_ms", 0) < prev[0] or prev[1]):
                run_idx[key0] = run_idx.get(key0, 0) + 1
            key = key0 + (f"run{run_idx.get(key0, 0)}",)
            last_t[key0] = (row.get("t_ms", 0), row.get("final") == 1)
        if key not in segs:
            segs[key] = []
            order.append(key)
        segs[key].append(row)
    return [(k, segs[k]) for k in order]


def seg_metrics(rows):
    last = rows[-1]
    finals = [x for x in rows if x.get("final") == 1]
    end = finals[-1] if finals else last
    hist = [int(x) for x in str(end.get("hist", "")).split("/") if x.isdigit()]
    rcvd, lost, sent = end.get("rcvd", 0), end.get("lost", 0), end.get("sent", 0)
    m = {
        "flow": end["flow"], "src": end["src"], "final": end.get("final") == 1,
        "t_start_s": rows[0]["host_t"] - rows[0].get("t_ms", 0) / 1000.0
        if rows[0]["host_t"] is not None else None,
        "duration_s": end.get("t_ms", 0) / 1000.0,
        "sent": sent, "rcvd": rcvd, "lost": lost, "dup": end.get("dup", 0),
        "reord": end.get("reord", 0), "stale": end.get("stale", 0),
        "loss_pct": r(100.0 * lost / (rcvd + lost)) if rcvd + lost else None,
        "rtt_n": end.get("rtt_n", 0),
        "rtt_min_ms": r(end.get("rtt_min_us", 0) / 1000.0) if end.get("rtt_n") else None,
        "rtt_avg_ms": r(end.get("rtt_avg_us", 0) / 1000.0) if end.get("rtt_n") else None,
        "rtt_max_ms": r(end.get("rtt_max_us", 0) / 1000.0) if end.get("rtt_n") else None,
        "jitter_ms": r(end.get("jitter_us", 0) / 1000.0),
        "rtt_p50_ms": hist_pct_clamped(hist, 50, end),
        "rtt_p95_ms": hist_pct_clamped(hist, 95, end),
        "rtt_p99_ms": hist_pct_clamped(hist, 99, end),
        "rtt_pct_source": "hist" if hist and end.get("rtt_n") else None,
        "hist": hist,
    }
    for k, v in end.items():
        if k not in m and k not in ("line", "host_t", "t_ms", "hist") and k not in (
                "rtt_min_us", "rtt_avg_us", "rtt_max_us", "jitter_us", "loss_ppm"):
            m[k] = v
    return m


def attach_events(summary_flows, events):
    """Client start parameters, TCP connection history.

    Each segment gets the latest ev=start of its (src, flow) at or before its first STATS line,
    and the TCP events from that start up to the next one, so restarted clients (node A restarts
    its services on every link-up) keep their own parameters and history.
    """
    starts = {}
    for e in events:
        if e.get("ev") == "start":
            starts.setdefault((e["src"], e["flow"]), []).append(e)
    for m in summary_flows:
        own = starts.get((m["src"], m["flow"]), [])
        first = m.get("_first_line")
        st = None
        nxt = None
        for e in own:
            if first is None or e["line"] <= first:
                st = e
            elif nxt is None:
                nxt = e
        lo = st["line"] if st else 0
        hi = nxt["line"] if nxt else float("inf")
        if st:
            m["session"] = st.get("session")
            m["payload"] = st.get("payload")
            m["rate"] = st.get("rate_pps", st.get("rate_mps"))
            m["dst"] = st.get("dst")
            cfg_ms = st.get("duration_ms") or 0
            # Active sending time: the configured duration when set (the final line also
            # includes the drain / final-ACK wait), else the last report time.
            m["active_s"] = cfg_ms / 1000.0 if cfg_ms else m["duration_s"]
            if m["active_s"] and m.get("payload") is not None:
                m["goodput_kbps"] = r(m["rcvd"] * m["payload"] * 8 / m["active_s"] / 1000.0)
        if "connects" in m:
            m["reconnects"] = max(0, m["connects"] - 1)
            m["acked_pct"] = r(100.0 * m["rcvd"] / m["sent"]) if m["sent"] else None
            hist = []
            for e in events:
                if e["src"] != m["src"] or e["flow"] != m["flow"]:
                    continue
                if not lo <= e["line"] < hi:
                    continue
                if e.get("ev") in ("disconnected", "connected", "connect_failed"):
                    item = {"ev": e["ev"], "t_s": e["host_t"], "t_ms": e.get("t_ms")}
                    for k in ("reason", "attempt", "outage_ms", "connect_ms", "unacked", "err",
                              "retry_ms"):
                        if k in e:
                            item[k] = e[k]
                    hist.append(item)
            m["connection_events"] = hist


RUN_ADDITIVE = ("sent", "rcvd", "lost", "dup", "reord", "stale", "rtt_n", "tx_err", "bad",
                "foreign", "corrupt", "connects", "disconnects", "stalls", "connect_fail",
                "bad_status", "proto_err", "crc_err", "resyncs")


def aggregate_runs(ms):
    """Combine several runs of one client flow (same src and flow name) into one flow summary.

    Counters are summed and RTT min/max/avg combined exactly; percentiles come from the summed
    histograms and jitter is the rtt_n-weighted mean of the runs.
    """
    if len(ms) == 1:
        return ms[0]
    a = dict(ms[-1])
    for k in RUN_ADDITIVE:
        if any(k in m for m in ms):
            a[k] = sum(m.get(k) or 0 for m in ms)
    a["loss_pct"] = r(100.0 * a["lost"] / (a["rcvd"] + a["lost"])) if a["rcvd"] + a["lost"] else None
    timed = [m for m in ms if m.get("rtt_n")]
    n = sum(m["rtt_n"] for m in timed)
    if timed:
        a["rtt_min_ms"] = min(m["rtt_min_ms"] for m in timed)
        a["rtt_max_ms"] = max(m["rtt_max_ms"] for m in timed)
        a["rtt_avg_ms"] = r(sum(m["rtt_avg_ms"] * m["rtt_n"] for m in timed) / n)
        a["jitter_ms"] = r(sum((m.get("jitter_ms") or 0) * m["rtt_n"] for m in timed) / n)
    hists = [m.get("hist") or [] for m in ms]
    width = max(len(h) for h in hists)
    hist = [sum(h[i] for h in hists if i < len(h)) for i in range(width)]
    end = {"rtt_n": n, "rtt_min_us": (a.get("rtt_min_ms") or 0) * 1000.0,
           "rtt_max_us": (a.get("rtt_max_ms") or 0) * 1000.0}
    a["hist"] = hist
    for p in (50, 95, 99):
        a[f"rtt_p{p}_ms"] = hist_pct_clamped(hist, p, end)
    a["rtt_pct_source"] = "hist" if hist and n else None
    gaps = [m["longest_gap_s"] for m in ms if m.get("longest_gap_s") is not None]
    if gaps:
        a["longest_gap_s"] = max(gaps)
    a["t_start_s"] = ms[0].get("t_start_s")
    a["duration_s"] = r(sum(m.get("duration_s") or 0 for m in ms))
    if all(m.get("active_s") is not None for m in ms):
        a["active_s"] = sum(m["active_s"] for m in ms)
        if a["active_s"] and a.get("payload") is not None:
            a["goodput_kbps"] = r(a["rcvd"] * a["payload"] * 8 / a["active_s"] / 1000.0)
    if "connects" in a:
        a["reconnects"] = max(0, a["connects"] - 1)
        a["acked_pct"] = r(100.0 * a["rcvd"] / a["sent"]) if a["sent"] else None
        a["connection_events"] = [e for m in ms for e in m.get("connection_events", [])]
    a["sessions"] = [m.get("session") for m in ms]
    a["runs"] = len(ms)
    a["restarts"] = len(ms) - 1
    return a


def ping_metrics(pings, totals, dsts=None):
    out = {}
    by = {}
    for p in pings:
        by.setdefault(p["flow"], []).append(p)
    for flow in list(by) + [f for f in totals if f not in by]:
        rows = by.get(flow, [])
        tot = totals.get(flow)
        replies = {}
        for p in rows:
            if p["rtt_ms"] is not None:
                replies.setdefault((p.get("run", 0), p["seq"]), p)
        # Highest seq of each probe run (seq can restart when node A recreates its probe).
        top = {}
        for p in rows:
            if p["seq"] is not None:
                k = p.get("run", 0)
                top[k] = max(top.get(k, 0), p["seq"])
        tx = sum(top.values())
        if tot is not None:
            # Replies after the last summary (log cut mid-run) are not in its "sent" yet.
            tx = max(tot["sent"], tx)
        rtts = [p["rtt_ms"] for p in sorted(replies.values(),
                                             key=lambda x: (x.get("run", 0), x["seq"]))]
        rx = len(replies)
        jit = None
        if len(rtts) > 1:
            jit = sum(abs(a - b) for a, b in zip(rtts[1:], rtts[:-1])) / (len(rtts) - 1)
        rmin = r(min(rtts)) if rtts else None
        ravg = r(sum(rtts) / len(rtts)) if rtts else None
        rmax = r(max(rtts)) if rtts else None
        source = "samples"
        if not rows and tot is not None:
            # Summary lines only (node A with CONFIG_LAB_A_PING_VERBOSE=n): no percentiles.
            rx = tot["rcvd"]
            rmin, ravg, rmax = (r(tot.get(k)) for k in ("rtt_min_ms", "rtt_avg_ms", "rtt_max_ms"))
            source = None
        out[flow] = {
            "flow": flow, "dst": rows[0]["dst"] if rows else (dsts or {}).get(flow),
            "tx": tx, "rx": rx,
            "lost": max(0, tx - rx), "loss_pct": r(100.0 * (tx - rx) / tx) if tx else None,
            "rtt_min_ms": rmin, "rtt_avg_ms": ravg,
            "rtt_p50_ms": r(percentile(rtts, 50)), "rtt_p95_ms": r(percentile(rtts, 95)),
            "rtt_p99_ms": r(percentile(rtts, 99)),
            "rtt_max_ms": rmax,
            "jitter_ms": r(jit), "rtt_pct_source": source,
        }
    return out


# --------------------------------------------------------------------------------------------
# pcap (simulation): exact per-packet UDP echo RTT from the echoed ts_us
# --------------------------------------------------------------------------------------------


def pcap_udp_rtts(path, mono_minus_real, t0_mono, port=5000):
    """Yield (t_s, session_hex, seq, rtt_ms) for each ECHO_RSP received on A.

    RTT = capture time (CLOCK_REALTIME, converted to CLOCK_MONOTONIC) minus the request's
    ts_us (lab_time_us(), CLOCK_MONOTONIC). The app sees the reply a few us later.
    """
    with open(path, "rb") as f:
        hdr = f.read(24)
        if len(hdr) < 24:
            return
        magic = struct.unpack("<I", hdr[:4])[0]
        if magic in (0xA1B2C3D4, 0xA1B23C4D):
            endian = "<"
        else:
            endian = ">"
            magic = struct.unpack(">I", hdr[:4])[0]
        nano = magic == 0xA1B23C4D
        linktype = struct.unpack(endian + "I", hdr[20:24])[0]
        if linktype != 1:
            raise ValueError(f"{path}: unsupported link type {linktype}")
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                break
            ts_s, ts_frac, incl, _orig = struct.unpack(endian + "IIII", rh)
            data = f.read(incl)
            t_real = ts_s + ts_frac / (1e9 if nano else 1e6)
            if len(data) < 14 + 20 + 8 + 24 or data[12:14] != b"\x08\x00":
                continue
            ihl = (data[14] & 0x0F) * 4
            if data[14 + 9] != 17:
                continue
            u = 14 + ihl
            sport = struct.unpack(">H", data[u:u + 2])[0]
            if sport != port:
                continue
            lp = data[u + 8:]
            if len(lp) < 24:
                continue
            magic_lp, ver, typ = struct.unpack(">IBB", lp[:6])
            if magic_lp != LINKPROTO_MAGIC or ver != 1 or typ != 2:
                continue
            seq, session, ts_us = struct.unpack(">IIQ", lp[8:24])
            t_mono = t_real + mono_minus_real
            yield (t_mono - t0_mono, f"{session:08x}", seq, (t_mono - ts_us / 1e6) * 1000.0)


# --------------------------------------------------------------------------------------------
# Scenario-level analysis
# --------------------------------------------------------------------------------------------


def link_windows(events):
    wins = []
    down = {}
    for e in events:
        if e["flow"] != "netsim":
            continue
        if e.get("ev") == "link_down":
            down[e.get("hop")] = e["host_t"]
        elif e.get("ev") == "link_up" and e.get("hop") in down:
            wins.append({"hop": e.get("hop"), "down_s": down.pop(e.get("hop")), "up_s": e["host_t"]})
    return wins


def model_capacity_pps(meta, payload):
    sc = meta["scenario"]
    med = sc["medium"]
    frame = 14 + 20 + 8 + 32 + payload + meta["air_overhead_bytes"]
    airtime_s = frame * 8 / (med["phy_rate_kbps"] * 1000.0)
    n_air = 4 if sc["topology"] == "relay" else 2
    return 1.0 / (n_air * airtime_s)


def resolve(d, path):
    cur = d
    for part in path.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return None
        cur = cur[part]
    return cur


def run_checks(summary, expect):
    out = []
    for e in expect:
        v = resolve(summary, e["metric"])
        ok = v is not None
        if ok and "min" in e:
            ok = v >= e["min"]
        if ok and "max" in e:
            ok = ok and v <= e["max"]
        rng = (f">= {e['min']}" if "min" in e else "") + (" and " if "min" in e and "max" in e else "") + (
            f"<= {e['max']}" if "max" in e else "")
        out.append({"metric": e["metric"], "value": v, "expect": rng, "ok": bool(ok)})
    return out


def analyse(rec, meta=None, rtt_samples=None):
    segs = segment_stats(rec.stats)
    seg_list = []
    for key, rows in segs:
        m = seg_metrics(rows)
        m["_first_line"] = rows[0]["line"]
        # Client runs are keyed "runN"; server sessions by the client's session id.
        m["_client"] = key[2].startswith("run")
        seg_list.append(m)
    attach_events(seg_list, rec.events)

    # Exact RTT percentiles (simulation): pcap samples per client session.
    if rtt_samples:
        by_sess = {}
        for t, sess, _seq, rtt in rtt_samples:
            by_sess.setdefault(sess, []).append((t, rtt))
        for m in seg_list:
            sess = m.get("session")
            if "peer" in m or not sess or sess not in by_sess or not m.get("rtt_n"):
                continue
            vals = [x[1] for x in by_sess[sess]]
            m["rtt_p50_ms"] = r(percentile(vals, 50))
            m["rtt_p95_ms"] = r(percentile(vals, 95))
            m["rtt_p99_ms"] = r(percentile(vals, 99))
            m["rtt_pct_source"] = "pcap"
            times = sorted(x[0] for x in by_sess[sess])
            if len(times) > 1:
                m["longest_gap_s"] = r(max(b - a for a, b in zip(times[:-1], times[1:])))

    # Flow summary: every run of a client flow combined (node A restarts its clients on each
    # link-up, so a serial log across a relay drop has several), the last session of a server.
    runs = {}
    for m in seg_list:
        if m["_client"]:
            runs.setdefault((m["src"], m["flow"]), []).append(m)
    flows = {}
    for m in seg_list:
        if m["_client"]:
            flows[m["flow"]] = aggregate_runs(runs[(m["src"], m["flow"])])
        else:
            flows[m["flow"]] = m
    for m in list(seg_list) + list(flows.values()):
        m.pop("_first_line", None)
        m.pop("_client", None)
    flows.update(ping_metrics(rec.pings, rec.ping_totals, rec.ping_dst))

    summary = {"flows": flows, "segments": [
        {k: v for k, v in m.items() if k not in ("hist", "connection_events")} for m in seg_list]}
    derived = {}

    udp = flows.get("udp")
    if udp and udp.get("longest_gap_s") is not None:
        derived["udp_longest_gap_s"] = udp["longest_gap_s"]
    if udp and flows.get("udp_srv"):
        srv = flows["udp_srv"]
        derived["udp_uplink_loss_pct"] = srv.get("loss_pct")
        if udp.get("loss_pct") is not None and srv.get("loss_pct") is not None:
            # Round trip = uplink then downlink: (1-rt) = (1-up)(1-down)
            up, rt = srv["loss_pct"] / 100.0, udp["loss_pct"] / 100.0
            derived["udp_downlink_loss_pct_est"] = r(100.0 * (1 - (1 - rt) / (1 - up))) if up < 1 else None
    if udp and flows.get("udp_relay") and udp.get("rtt_p50_ms") and flows["udp_relay"].get("rtt_p50_ms"):
        derived["relay_rtt_over_hop_rtt"] = r(udp["rtt_p50_ms"] / flows["udp_relay"]["rtt_p50_ms"])

    wins = link_windows(rec.events)
    if wins:
        summary["link_windows"] = wins
        tcp = flows.get("tcp")
        if tcp is not None:
            ups = [w["up_s"] for w in wins]
            conns = [e for e in tcp.get("connection_events", []) if e["ev"] == "connected"
                     and e["t_s"] is not None]
            rec_times = []
            for up in ups:
                after = [c["t_s"] for c in conns if c["t_s"] >= up - 0.001]
                if after:
                    rec_times.append(after[0] - up)
            if rec_times:
                derived["tcp_recovery_after_link_up_s"] = r(max(rec_times))
            discs = [e for e in tcp.get("connection_events", []) if e["ev"] == "disconnected"
                     and e["t_s"] is not None]
            if discs:
                derived["tcp_detect_after_link_down_s"] = r(discs[0]["t_s"] - wins[0]["down_s"])
            outs = [e.get("outage_ms") for e in conns if e.get("outage_ms")]
            if outs:
                derived["tcp_outage_ms_max"] = max(outs)

    # Rate sweep: flows named <prefix>_<rate>.
    sweep_flows = sorted((m for m in flows.values() if re.match(r"^[a-z]+_\d{4}$", m["flow"])
                          and "rtt_n" in m), key=lambda m: int(m["flow"].split("_")[1]))
    if sweep_flows:
        steps = []
        for m in sweep_flows:
            rate = int(m["flow"].split("_")[1])
            delivered = m["rcvd"] / max(m.get("active_s") or m["duration_s"] or 1, 1e-9)
            steps.append({"flow": m["flow"], "rate_pps": rate, "payload": m.get("payload"),
                          "sent": m["sent"], "rcvd": m["rcvd"], "loss_pct": m["loss_pct"],
                          "rtt_p50_ms": m["rtt_p50_ms"], "rtt_p95_ms": m["rtt_p95_ms"],
                          "rtt_max_ms": m["rtt_max_ms"], "delivered_pps": r(delivered, 2),
                          "goodput_kbps_per_dir": r(delivered * (m.get("payload") or 0) * 8 / 1000.0)})
        sw = {"steps": steps, "first_loss_pct": steps[0]["loss_pct"], "last_loss_pct": steps[-1]["loss_pct"]}
        ok_steps = [s for s in steps if s["loss_pct"] is not None and s["loss_pct"] < 1.0]
        sw["max_rate_loss_below_1pct"] = max((s["rate_pps"] for s in ok_steps), default=None)
        sw["max_delivered_pps"] = max(s["delivered_pps"] for s in steps)
        sw["max_goodput_kbps_per_dir"] = max(s["goodput_kbps_per_dir"] or 0 for s in steps)
        if meta is not None and steps[0]["payload"] is not None:
            cap = model_capacity_pps(meta, steps[0]["payload"])
            sw["model_capacity_pps"] = r(cap, 2)
            sw["knee_over_model"] = r(sw["max_delivered_pps"] / cap)
        summary["sweep"] = sw

    summary["derived"] = derived
    if meta is not None:
        sc = meta["scenario"]
        summary["scenario"] = {k: sc.get(k) for k in ("name", "title", "description", "topology",
                                                     "duration_s")}
        summary["model"] = {"medium": sc["medium"], "hops": sc["hops"],
                            "relay_fwd_delay_ms": sc.get("relay_fwd_delay_ms"),
                            "root_drops_group_tx": sc.get("root_drops_group_tx"),
                            "events": sc.get("events", []), "air_overhead_bytes": meta["air_overhead_bytes"],
                            "timer_comp_ms": meta.get("timer_comp_ms")}
        summary["netsim"] = {"air_counters": meta.get("air_counters"),
                             "root_group_tx_dropped": meta.get("root_group_tx_dropped"),
                             "root_group_selftest_dropped": meta.get("root_group_selftest_dropped"),
                             "kernel": meta.get("kernel")}
        summary["checks"] = run_checks(summary, sc.get("expect", []))
    return summary


# --------------------------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------------------------


def write_csv(path, rows, first_cols):
    cols = list(first_cols)
    for row in rows:
        for k in row:
            if k not in cols:
                cols.append(k)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for row in rows:
            w.writerow(row)


def print_table(summary, out=sys.stdout):
    cols = ["flow", "sent", "rcvd", "lost", "loss_pct", "rtt_p50_ms", "rtt_p95_ms", "rtt_max_ms",
            "jitter_ms", "runs"]
    print(" ".join(f"{c:>12}" for c in cols), file=out)
    for name in sorted(summary["flows"]):
        m = summary["flows"][name]
        vals = [m.get(c, m.get({"sent": "tx", "rcvd": "rx"}.get(c, c))) for c in cols]
        print(" ".join(f"{'-' if v is None else v!s:>12}" for v in vals), file=out)
    for c in summary.get("checks", []):
        print(f"check {'PASS' if c['ok'] else 'FAIL'}: {c['metric']} = {c['value']} (expect {c['expect']})",
              file=out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("-o", "--out", help="output directory (default: next to the first log)")
    ap.add_argument("--sim-dir", help="netsim result directory (reads *.log, meta.json, pcap)")
    ap.add_argument("--pcap", help="override the pcap path for --sim-dir")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    rec = Records()
    meta = None
    samples = None
    if a.sim_dir:
        logs = sorted(glob.glob(os.path.join(a.sim_dir, "*.log")) +
                      glob.glob(os.path.join(a.sim_dir, "*.log.gz")))
        logs = [p for p in logs if not os.path.basename(p).startswith("tcpdump")]
        out = a.out or a.sim_dir
        with open(os.path.join(a.sim_dir, "meta.json"), encoding="utf-8") as f:
            meta = json.load(f)
        pcap = a.pcap or meta.get("pcap")
        clk = meta["clock"]
        rtt_csv = os.path.join(out, "udp_rtt.csv.gz")
        if pcap and os.path.exists(pcap):
            mono_minus_real = clk["t0_mono"] - clk["t0_real"]
            samples = list(pcap_udp_rtts(pcap, mono_minus_real, clk["t0_mono"]))
            with gzip.open(rtt_csv, "wt", encoding="utf-8", newline="") as f:
                w = csv.writer(f)
                w.writerow(["t_s", "session", "seq", "rtt_ms"])
                for t, s, q, rt in samples:
                    w.writerow([f"{t:.6f}", s, q, f"{rt:.3f}"])
        elif os.path.exists(rtt_csv):
            with gzip.open(rtt_csv, "rt", encoding="utf-8") as f:
                rd = csv.DictReader(f)
                samples = [(float(x["t_s"]), x["session"], int(x["seq"]), float(x["rtt_ms"])) for x in rd]
    else:
        logs = a.logs
        if not logs:
            ap.error("give log files or --sim-dir")
        out = a.out or os.path.dirname(os.path.abspath(logs[0]))
    os.makedirs(out, exist_ok=True)
    for p in logs:
        name = os.path.basename(p)
        rec.feed_file(p, src=name.split(".log")[0])

    summary = analyse(rec, meta, samples)
    write_csv(os.path.join(out, "stats.csv"), rec.stats, ["src", "line", "host_t", "flow", "t_ms"])
    write_csv(os.path.join(out, "events.csv"), rec.events, ["src", "line", "host_t", "flow", "t_ms", "ev"])
    write_csv(os.path.join(out, "ping.csv"), rec.pings,
              ["src", "line", "host_t", "flow", "t_ms", "dst", "seq", "rtt_ms", "ttl"])
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    if not a.quiet:
        print_table(summary)
    failed = [c for c in summary.get("checks", []) if not c["ok"]]
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
