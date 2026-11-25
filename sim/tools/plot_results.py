#!/usr/bin/env python3
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
"""Plots for parsed lab results (matplotlib, headless).

    plot_results.py DIR [DIR ...]          per-result plots into each DIR
    plot_results.py --compare RESULTS_DIR  cross-scenario plots into RESULTS_DIR

DIR is any directory that parse_logs.py wrote (stats.csv, events.csv, ping.csv, summary.json,
optionally udp_rtt.csv.gz), so this works for hardware captures too.
"""

import argparse
import csv
import gzip
import json
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT2 = "#52514e"
GRID = "#e6e5e0"
BAND = "#52514e"
# Categorical slots in fixed order; each entity keeps its slot in every chart.
C_UDP_B = "#2a78d6"
C_PING_B = "#eb6834"
C_UDP_R = "#1baf7a"
C_TCP = "#6250d6"
C_PING_R = "#eda100"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": TEXT2, "xtick.color": TEXT2, "ytick.color": TEXT2,
    "text.color": TEXT, "axes.titlecolor": TEXT, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 9, "axes.titlesize": 10, "axes.titleweight": "bold", "legend.frameon": False,
    "lines.linewidth": 1.6,
})


def read_csv(path):
    if not os.path.exists(path):
        return []
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def load(d):
    with open(os.path.join(d, "summary.json"), encoding="utf-8") as f:
        summary = json.load(f)
    return {
        "summary": summary,
        "stats": read_csv(os.path.join(d, "stats.csv")),
        "events": read_csv(os.path.join(d, "events.csv")),
        "ping": read_csv(os.path.join(d, "ping.csv")),
        "rtt": read_csv(os.path.join(d, "udp_rtt.csv.gz")),
    }


def shade_windows(ax, summary, label=True):
    for i, w in enumerate(summary.get("link_windows", [])):
        ax.axvspan(w["down_s"], w["up_s"], color=BAND, alpha=0.10, lw=0,
                   label=f"{w['hop'].replace('_', '-').upper()} link down" if label and i == 0 else None)


def session_of(summary, flow):
    m = summary["flows"].get(flow)
    return m.get("session") if m else None


def rtt_series(data, flow):
    sess = session_of(data["summary"], flow)
    pts = [(fnum(x["t_s"]), fnum(x["rtt_ms"])) for x in data["rtt"] if x["session"] == sess]
    return [p for p in pts if None not in p]


def ping_series(data, flow):
    pts = [(fnum(x["host_t"]), fnum(x["rtt_ms"])) for x in data["ping"] if x["flow"] == flow]
    return [p for p in pts if None not in p]


def plot_timeline(d, data):
    s = data["summary"]
    title = s.get("scenario", {}).get("title") or os.path.basename(d)
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 5.6), sharex=True,
                                   gridspec_kw={"height_ratios": [3, 2]})
    series = [("udp", "UDP echo A→B", C_UDP_B, rtt_series(data, "udp")),
              ("ping_node_b", "ping A→B", C_PING_B, ping_series(data, "ping_node_b")),
              ("udp_relay", "UDP echo A→relay", C_UDP_R, rtt_series(data, "udp_relay"))]
    sweep = s.get("sweep")
    if sweep:
        sess = {m.get("session") for m in s["flows"].values()
                if m["flow"] in {x["flow"] for x in sweep["steps"]}}
        pts = [(fnum(x["t_s"]), fnum(x["rtt_ms"])) for x in data["rtt"] if x["session"] in sess]
        series[0] = ("udp", "UDP echo A→B (sweep)", C_UDP_B, [p for p in pts if None not in p])
    if not series[0][3]:
        # No pcap (hardware log): fall back to per-interval average RTT from STATS.
        series[0] = ("udp", "UDP echo A→B (interval avg)", C_UDP_B, interval_avg_rtt(data, "udp"))
    ymax = 0
    for _, label, color, pts in series:
        if not pts:
            continue
        xs, ys = zip(*pts)
        ax1.scatter(xs, ys, s=7, color=color, label=label, linewidths=0, alpha=0.85)
        ymax = max(ymax, sorted(ys)[int(0.995 * (len(ys) - 1))])
    shade_windows(ax1, s)
    ax1.set_ylabel("RTT (ms)")
    ax1.set_ylim(0, ymax * 1.35 if ymax else 1)
    ax1.set_title(f"{title}: round-trip time")
    ax1.legend(loc="upper right", ncol=4, markerscale=2)

    if sweep:
        plot_sweep_rate(ax2, data, series[0][3], sweep)
        shade_windows(ax2, s, label=False)
        ax2.set_xlabel("scenario time (s)")
        fig.tight_layout()
        fig.savefig(os.path.join(d, "timeline.png"), dpi=110)
        plt.close(fig)
        return
    for flow, label, color in (("udp", "UDP echo A→B (round trip)", C_UDP_B),
                               ("tcp", "TCP messages never ACKed", C_TCP)):
        rows = [x for x in data["stats"] if x["flow"] == flow and x["src"] == "node_a"]
        pts = [(fnum(x["host_t"]), fnum(x["lost"])) for x in rows]
        pts = [p for p in pts if None not in p]
        if pts:
            xs, ys = zip(*pts)
            ax2.step(xs, ys, where="post", color=color, label=label)
    shade_windows(ax2, s, label=False)
    for e in data["events"]:
        if e["flow"] == "tcp" and e["ev"] in ("disconnected", "connected") and fnum(e["host_t"]):
            if e["ev"] == "connected" and fnum(e.get("outage_ms")) in (None, 0.0):
                continue
            t = fnum(e["host_t"])
            ax2.axvline(t, color=TEXT2, lw=0.8, ls="--")
            ax2.annotate("TCP " + ("drop" if e["ev"] == "disconnected" else "reconnected"),
                         (t, 1), xycoords=("data", "axes fraction"), xytext=(3, -12),
                         textcoords="offset points", fontsize=8, color=TEXT2,
                         bbox=dict(fc=SURFACE, ec="none", pad=1))
    ax2.set_ylabel("cumulative lost")
    ax2.set_xlabel("scenario time (s)")
    ax2.set_title("Loss")
    ax2.legend(loc="upper left")
    fig.tight_layout()
    fig.savefig(os.path.join(d, "timeline.png"), dpi=110)
    plt.close(fig)


def plot_sweep_rate(ax, data, pts, sweep):
    """Echoes received per second at A, with the offered request rate of each step."""
    if pts:
        bins = {}
        for t, _ in pts:
            bins[int(t)] = bins.get(int(t), 0) + 1
        xs = sorted(bins)
        ax.step(xs, [bins[x] for x in xs], where="post", color=C_UDP_B, label="echoes received / s")
    starts = {e["flow"]: fnum(e["host_t"]) for e in data["events"] if e["ev"] == "start"}
    for st in sweep["steps"]:
        t0 = starts.get(st["flow"])
        if t0 is None:
            continue
        dur = (fnum(st.get("sent")) or 0) / st["rate_pps"]
        ax.hlines(st["rate_pps"], t0, t0 + dur, color=TEXT2, lw=1.2, ls="--",
                  label="offered requests / s" if st is sweep["steps"][0] else None)
    ax.set_ylabel("per second")
    ax.set_title("Offered vs delivered")
    ax.legend(loc="upper left")


def interval_avg_rtt(data, flow):
    rows = [x for x in data["stats"] if x["flow"] == flow]
    out = []
    prev = None
    for x in rows:
        n, tot = fnum(x["rtt_n"]), fnum(x["rtt_avg_us"])
        t = fnum(x["host_t"]) if fnum(x["host_t"]) is not None else fnum(x["t_ms"]) / 1000.0
        if n is None or tot is None:
            continue
        if prev and n > prev[0]:
            out.append((t, (n * tot - prev[0] * prev[1]) / (n - prev[0]) / 1000.0))
        prev = (n, tot)
    return out


def cdf(ax, values, color, label):
    if not values:
        return
    v = sorted(values)
    ys = [100.0 * (i + 1) / len(v) for i in range(len(v))]
    ax.step(v, ys, where="post", color=color, label=label)


def plot_cdf(d, data):
    s = data["summary"]
    title = s.get("scenario", {}).get("title") or os.path.basename(d)
    fig, ax = plt.subplots(figsize=(7, 4))
    for flow, label, color, pts in (
            ("udp", "UDP echo A→B", C_UDP_B, rtt_series(data, "udp")),
            ("ping_node_b", "ping A→B", C_PING_B, ping_series(data, "ping_node_b")),
            ("udp_relay", "UDP echo A→relay", C_UDP_R, rtt_series(data, "udp_relay")),
            ("ping_relay", "ping A→relay", C_PING_R, ping_series(data, "ping_relay"))):
        cdf(ax, [p[1] for p in pts], color, label)
    ax.set_xlabel("RTT (ms)")
    ax.set_ylabel("% of samples ≤ x")
    ax.set_ylim(0, 100)
    ax.set_xlim(left=0)
    ax.set_title(f"{title}: RTT distribution")
    ax.legend(loc="lower right")
    fig.tight_layout()
    fig.savefig(os.path.join(d, "rtt_cdf.png"), dpi=110)
    plt.close(fig)


def plot_sweep(d, data):
    sw = data["summary"].get("sweep")
    if not sw:
        return
    title = data["summary"].get("scenario", {}).get("title") or os.path.basename(d)
    steps = sw["steps"]
    rate = [x["rate_pps"] for x in steps]
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(7, 5.6), sharex=True)
    ax1.plot(rate, [x["loss_pct"] for x in steps], marker="o", ms=4, color=C_UDP_B)
    if sw.get("model_capacity_pps"):
        ax1.axvline(sw["model_capacity_pps"], color=TEXT2, lw=0.8, ls="--")
        ax1.annotate(f"model capacity {sw['model_capacity_pps']:.0f} echo/s",
                     (sw["model_capacity_pps"], 0.95), xycoords=("data", "axes fraction"),
                     xytext=(4, 0), textcoords="offset points", fontsize=8, color=TEXT2)
    ax1.set_ylabel("round-trip loss (%)")
    ax1.set_title(f"{title}: loss vs offered rate")
    ax2.plot(rate, [x["rtt_p50_ms"] for x in steps], marker="o", ms=4, color=C_UDP_B, label="p50")
    ax2.plot(rate, [x["rtt_p95_ms"] for x in steps], marker="o", ms=4, color=C_PING_B, label="p95")
    ax2.set_ylabel("RTT (ms)")
    ax2.set_xlabel(f"offered UDP echo rate (requests/s, {steps[0]['payload']} B payload)")
    ax2.set_title("RTT vs offered rate")
    ax2.legend(loc="upper left")
    fig.tight_layout()
    fig.savefig(os.path.join(d, "saturation.png"), dpi=110)
    plt.close(fig)


def plot_dir(d):
    data = load(d)
    if data["summary"].get("sweep"):
        plot_sweep(d, data)
    plot_timeline(d, data)
    plot_cdf(d, data)


def plot_compare(root):
    dirs = sorted(x for x in os.listdir(root) if os.path.exists(os.path.join(root, x, "summary.json")))
    loaded = {x: load(os.path.join(root, x)) for x in dirs}

    order = [("baseline_direct", C_UDP_B), ("relay_nominal", C_PING_B), ("relay_lossy", C_UDP_R)]
    fig, ax = plt.subplots(figsize=(7, 4))
    for name, color in order:
        if name in loaded:
            cdf(ax, [p[1] for p in rtt_series(loaded[name], "udp")], color, name)
    ax.set_xlabel("UDP echo RTT A→B (ms)")
    ax.set_ylabel("% of samples ≤ x")
    ax.set_ylim(0, 100)
    ax.set_xlim(left=0)
    ax.set_title("UDP echo RTT: direct vs through the relay")
    ax.legend(loc="lower right")
    fig.tight_layout()
    fig.savefig(os.path.join(root, "rtt_compare.png"), dpi=110)
    plt.close(fig)

    sweeps = [(n, c) for n, c in (("direct_saturation", C_UDP_B), ("relay_saturation", C_PING_B))
              if n in loaded and loaded[n]["summary"].get("sweep")]
    if sweeps:
        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(7, 5.6), sharex=True)
        for n, c in sweeps:
            st = loaded[n]["summary"]["sweep"]["steps"]
            off = [x["rate_pps"] * x["payload"] * 8 / 1000.0 for x in st]
            ax1.plot(off, [x["goodput_kbps_per_dir"] for x in st], marker="o", ms=4, color=c, label=n)
            ax2.plot(off, [x["loss_pct"] for x in st], marker="o", ms=4, color=c, label=n)
        ax1.set_ylabel("delivered (kbit/s per direction)")
        ax1.set_title("UDP echo goodput vs offered load")
        ax1.legend(loc="upper left")
        ax2.set_ylabel("round-trip loss (%)")
        ax2.set_xlabel("offered load (kbit/s per direction, 1000 B payload)")
        ax2.set_title("Loss vs offered load")
        fig.tight_layout()
        fig.savefig(os.path.join(root, "saturation_compare.png"), dpi=110)
        plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="*")
    ap.add_argument("--compare")
    a = ap.parse_args()
    for d in a.dirs:
        plot_dir(d)
    if a.compare:
        plot_compare(a.compare)


if __name__ == "__main__":
    main()
