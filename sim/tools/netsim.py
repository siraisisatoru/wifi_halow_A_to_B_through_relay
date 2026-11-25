#!/usr/bin/env python3
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
"""Network-namespace model of the A -> relay -> B HaLow lab (run as root inside halow-hostsim).

Topology (all in one container):

    hlA: eth0 192.168.50.10 ── veth ── hlR: ra ┐
                                              br0 192.168.50.2 (relay; no IP when "direct")
    hlB: eth0 192.168.50.1  ── veth ── hlR: rb ┘

Link model, built from a scenario file (sim/scenarios/*.json):

* Per hop and direction, a netem qdisc on the transmitter's egress: one-way latency
  (host/driver/firmware), jitter and residual frame loss after MAC retries. A 1 Gbit/s netem rate
  keeps the queue FIFO, so jitter never reorders frames (802.11 delivers in order per TID).
* One shared airtime queue in hlR (netem rate + per-frame overhead on veth air0) models the single
  2 MHz channel. Every over-the-air transmission is pushed through it once: frames the relay
  receives (sent by A or B) and frames the relay transmits. A relayed frame is therefore charged
  twice, which is the store-and-forward halving of a single-radio repeater. In the "direct"
  topology hlR is only the medium (no IP, no relay transmissions) and every frame is charged once.
  tc marks steer the frames: skbedit sets a stage mark, mirred redirects to air0, and air1
  sends each frame back to the device and direction it came from.
* Optional model of mm-iot-sdk bug #10 (unpatched relay root drops its own group-addressed
  frames): a u32 filter on hlB egress drops frames with the multicast bit set in the DA.
* Events (link down/up, loss changes) are applied at scenario times with "tc qdisc change".

Subcommands:
    netsim.py calibrate --out calibration.json
    netsim.py run --scenario S.json --bin BUILD_DIR --out RESULT_DIR [--calibration C.json]
    netsim.py teardown
"""

import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import threading
import time

NS_A, NS_R, NS_B = "hlA", "hlR", "hlB"
IP_B, IP_R, IP_A = "192.168.50.1", "192.168.50.2", "192.168.50.10"
PREFIX = 24
MAC_A = "02:00:00:00:00:0a"
MAC_B = "02:00:00:00:00:01"
# Stands in for the relay's STA (device) MAC, which carries its IP on the EKH05. It is NOT the
# relay AP BSSID 02:48:4C:00:00:02: that is only the BSS node A associates to.
MAC_R = "02:00:00:00:00:02"

# Stage marks used by the airtime pipeline in hlR.
MARK_TX_RA, MARK_TX_RB, MARK_RX_RA, MARK_RX_RB = 1, 2, 3, 4

# netem on a hop always gets this rate so that its tfifo stays in order.
HOP_FIFO_RATE = "1gbit"


def log(msg):
    print(f"netsim: {msg}", file=sys.stderr, flush=True)


def sh(cmd, check=True, quiet=False):
    """Run a command (list or string). Returns CompletedProcess."""
    args = shlex.split(cmd) if isinstance(cmd, str) else cmd
    res = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if check and res.returncode != 0:
        raise RuntimeError(f"command failed ({res.returncode}): {' '.join(args)}\n{res.stderr}")
    if not quiet and res.returncode != 0:
        log(f"(ignored) {' '.join(args)}: {res.stderr.strip()}")
    return res


def nsx(ns, cmd, **kw):
    return sh(f"ip netns exec {ns} {cmd}", **kw)


# --------------------------------------------------------------------------------------------
# Topology
# --------------------------------------------------------------------------------------------


def teardown():
    for ns in (NS_A, NS_R, NS_B):
        sh(f"ip netns del {ns}", check=False, quiet=True)
    # veth peers die with their namespace; give the kernel a moment to clean up.
    time.sleep(0.2)


def _ns_base(ns):
    sh(f"ip netns add {ns}")
    nsx(ns, "ip link set lo up")
    # IPv4 only, as in the firmware; keeps IPv6 ND/MLD noise off the modelled channel.
    nsx(ns, "sysctl -qw net.ipv6.conf.all.disable_ipv6=1")
    nsx(ns, "sysctl -qw net.ipv6.conf.default.disable_ipv6=1")


def _no_gso(ns, dev):
    # Frames must be MTU-sized when they hit the airtime model (no 64 KiB GSO super-frames).
    nsx(ns, f"ip link set dev {dev} gso_max_segs 1 gso_max_size 1500", check=False)


def build_topology(topology):
    teardown()
    for ns in (NS_A, NS_R, NS_B):
        _ns_base(ns)

    sh(f"ip link add eth0 netns {NS_A} address {MAC_A} type veth peer name ra netns {NS_R}")
    sh(f"ip link add eth0 netns {NS_B} address {MAC_B} type veth peer name rb netns {NS_R}")
    nsx(NS_R, f"ip link add br0 address {MAC_R} type bridge forward_delay 0 stp_state 0")
    nsx(NS_R, "ip link set ra master br0")
    nsx(NS_R, "ip link set rb master br0")
    nsx(NS_R, "ip link add air0 type veth peer name air1")
    for dev in ("ra", "rb", "air0", "air1"):
        _no_gso(NS_R, dev)
        nsx(NS_R, f"ip link set {dev} up")
    nsx(NS_R, "ip link set br0 up")
    for ns in (NS_A, NS_B):
        _no_gso(ns, "eth0")
        nsx(ns, "ip link set eth0 up")

    nsx(NS_A, f"ip addr add {IP_A}/{PREFIX} dev eth0")
    nsx(NS_B, f"ip addr add {IP_B}/{PREFIX} dev eth0")
    if topology == "relay":
        nsx(NS_R, f"ip addr add {IP_R}/{PREFIX} dev br0")
    elif topology != "direct":
        raise ValueError(f"unknown topology {topology!r}")


# --------------------------------------------------------------------------------------------
# Link model
# --------------------------------------------------------------------------------------------


def airtime_overhead_bytes(medium):
    """Per-frame overhead in bytes at the PHY rate, as netem's packetoverhead.

    The skb length already includes the 14-byte Ethernet header; the 802.11 MAC header, LLC/SNAP,
    CCMP and FCS (mac_overhead_bytes) replace it. Fixed per-frame time (preamble, SIFS, ACK, AIFS,
    mean backoff) is converted to bytes at the PHY rate.
    """
    rate_kbps = medium["phy_rate_kbps"]
    fixed = medium["frame_overhead_us"] * rate_kbps / 8000.0
    return int(round(fixed + medium["mac_overhead_bytes"] - 14))


def netem_args(delay_ms, jitter_ms, loss_pct, loss_corr_pct=0.0, extra_delay_ms=0.0, comp_ms=0.0,
               dist="paretonormal"):
    d = max(0.0, delay_ms + extra_delay_ms - comp_ms)
    args = [f"delay {d:.3f}ms"]
    if jitter_ms > 0:
        args.append(f"{jitter_ms:.3f}ms")
        if dist:
            args.append(f"distribution {dist}")
    if loss_pct > 0:
        args.append(f"loss random {loss_pct:.3f}%")
        if loss_corr_pct > 0:
            args.append(f"{loss_corr_pct:.1f}%")
    args.append(f"rate {HOP_FIFO_RATE}")
    args.append("limit 10000")
    return " ".join(args)


class LinkModel:
    """Owns the qdiscs; remembers each hop's nominal parameters so events can restore them."""

    def __init__(self, scenario, comp_ms):
        self.sc = scenario
        self.topology = scenario["topology"]
        self.comp_ms = comp_ms
        self.dist = scenario.get("jitter_distribution", "paretonormal")
        self.relay_fwd_ms = scenario.get("relay_fwd_delay_ms", 0.0) if self.topology == "relay" else 0.0
        self.hops = {}
        # hop name -> list of (ns, dev, extra_delay_ms) transmitters of that hop
        if self.topology == "relay":
            self.hop_devs = {
                "a_r": [(NS_A, "eth0", 0.0), (NS_R, "ra", self.relay_fwd_ms)],
                "r_b": [(NS_R, "rb", self.relay_fwd_ms), (NS_B, "eth0", 0.0)],
            }
        else:
            self.hop_devs = {"a_b": [(NS_A, "eth0", 0.0), (NS_B, "eth0", 0.0)]}
        self.state = {}
        self.group_selftest = None

    def _apply_hop(self, hop, verb, params):
        for ns, dev, extra in self.hop_devs[hop]:
            a = netem_args(params["delay_ms"], params.get("jitter_ms", 0.0), params.get("loss_pct", 0.0),
                           params.get("loss_corr_pct", 0.0), extra, self.comp_ms, self.dist)
            nsx(ns, f"tc qdisc {verb} dev {dev} root handle 1: netem {a}")
        self.state[hop] = dict(params)

    def setup(self):
        for hop in self.hop_devs:
            if hop not in self.sc["hops"]:
                raise ValueError(f"scenario lacks hop {hop!r} for topology {self.topology}")
            self.hops[hop] = dict(self.sc["hops"][hop])
            self._apply_hop(hop, "add", self.hops[hop])
        self._setup_air()
        if self.sc.get("root_drops_group_tx", False):
            # Bug #10 model: the relay root cannot transmit group-addressed frames.
            nsx(NS_B, "tc qdisc add dev eth0 clsact")
            nsx(NS_B, "tc filter add dev eth0 egress prio 1 protocol all u32 "
                      "match u8 0x01 0x01 at -14 action drop")
            # Self-test: one broadcast ping from B must be counted as dropped.
            nsx(NS_B, "ping -b -n -c 1 -W 0.3 192.168.50.255", check=False, quiet=True)
            self.group_selftest = self.group_drop_count()

    def _setup_air(self):
        m = self.sc["medium"]
        oh = airtime_overhead_bytes(m)
        nsx(NS_R, f"tc qdisc add dev air0 root handle 1: netem rate {m['phy_rate_kbps']}kbit {oh} "
                  f"limit {m['queue_frames']}")
        for dev in ("ra", "rb", "air1"):
            nsx(NS_R, f"tc qdisc add dev {dev} clsact")
        f = "tc filter add"

        def stage(dev, direction, mark):
            nsx(NS_R, f"{f} dev {dev} {direction} prio 1 protocol all u32 match mark {mark} 0xffffffff "
                      "action ok")
            nsx(NS_R, f"{f} dev {dev} {direction} prio 2 protocol all u32 match u32 0 0 "
                      f"action skbedit mark {mark} pipe action mirred egress redirect dev air0")

        # Frames received over the air by hlR (transmitted by A or B).
        stage("ra", "ingress", MARK_RX_RA)
        stage("rb", "ingress", MARK_RX_RB)
        nsx(NS_R, f"{f} dev air1 ingress prio 3 protocol all u32 match mark {MARK_RX_RA} 0xffffffff "
                  "action mirred ingress redirect dev ra")
        nsx(NS_R, f"{f} dev air1 ingress prio 4 protocol all u32 match mark {MARK_RX_RB} 0xffffffff "
                  "action mirred ingress redirect dev rb")
        if self.topology == "relay":
            # Frames the relay transmits (forwarded or its own) take airtime a second time.
            stage("ra", "egress", MARK_TX_RA)
            stage("rb", "egress", MARK_TX_RB)
            nsx(NS_R, f"{f} dev air1 ingress prio 1 protocol all u32 match mark {MARK_TX_RA} "
                      "0xffffffff action mirred egress redirect dev ra")
            nsx(NS_R, f"{f} dev air1 ingress prio 2 protocol all u32 match mark {MARK_TX_RB} "
                      "0xffffffff action mirred egress redirect dev rb")
        nsx(NS_R, f"{f} dev air1 ingress prio 9 protocol all u32 match u32 0 0 action drop")

    def set_hop(self, hop, **changes):
        params = dict(self.state[hop])
        params.update(changes)
        self._apply_hop(hop, "change", params)

    def restore_hop(self, hop):
        self._apply_hop(hop, "change", self.hops[hop])

    def stats(self):
        out = {}
        for ns, devs in ((NS_A, ["eth0"]), (NS_B, ["eth0"]), (NS_R, ["ra", "rb", "air0"])):
            for dev in devs:
                out[f"{ns}/{dev}"] = nsx(ns, f"tc -s qdisc show dev {dev}", check=False).stdout
        out[f"{NS_B}/eth0 egress filters"] = nsx(NS_B, "tc -s filter show dev eth0 egress",
                                                  check=False).stdout
        return out

    def air_counters(self):
        txt = nsx(NS_R, "tc -s -j qdisc show dev air0", check=False).stdout
        try:
            for q in json.loads(txt):
                if q.get("kind") == "netem":
                    return {"sent_pkts": q.get("packets"), "sent_bytes": q.get("bytes"),
                            "dropped": q.get("drops"), "overlimits": q.get("overlimits")}
        except (ValueError, TypeError):
            pass
        return {}

    def group_drop_count(self):
        if not self.sc.get("root_drops_group_tx", False):
            return None
        txt = nsx(NS_B, "tc -s -j filter show dev eth0 egress", check=False).stdout
        try:
            for flt in json.loads(txt):
                for act in flt.get("options", {}).get("actions", []):
                    if act.get("control_action", {}).get("type") == "drop" or act.get("kind") == "gact":
                        return act.get("stats", {}).get("packets")
        except (ValueError, TypeError, AttributeError):
            pass
        return None


# --------------------------------------------------------------------------------------------
# Processes and logging
# --------------------------------------------------------------------------------------------


class Clock:
    def __init__(self):
        self.t0_mono = time.monotonic()
        self.t0_real = time.time()

    def now(self):
        return time.monotonic() - self.t0_mono


class Proc:
    """A process in a namespace whose stdout/stderr lines are written with a scenario timestamp."""

    def __init__(self, name, ns, argv, logpath, clock, append=False):
        self.name = name
        self.clock = clock
        self.logf = open(logpath, "a" if append else "w", encoding="utf-8")
        self.lock = threading.Lock()
        full = ["ip", "netns", "exec", ns] + argv
        self.write_line(f"# {name}: {' '.join(shlex.quote(a) for a in full)}")
        self.p = subprocess.Popen(full, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                  bufsize=1, errors="replace")
        self.threads = [threading.Thread(target=self._pump, args=(s,), daemon=True)
                        for s in (self.p.stdout, self.p.stderr)]
        for t in self.threads:
            t.start()

    def write_line(self, line):
        with self.lock:
            self.logf.write(f"[{self.clock.now():10.6f}] {line}\n")
            self.logf.flush()

    def _pump(self, stream):
        for line in stream:
            self.write_line(line.rstrip("\n"))

    def signal(self, sig=signal.SIGTERM):
        if self.p.poll() is None:
            # ip netns exec execs the target, so the pid is the app itself.
            self.p.send_signal(sig)

    def wait(self, timeout=None):
        try:
            rc = self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            rc = self.p.wait()
        for t in self.threads:
            t.join(timeout=2)
        self.write_line(f"# {self.name}: exit {rc}")
        self.logf.close()
        return rc


def kv_args(d):
    out = []
    for k, v in d.items():
        if isinstance(v, bool):
            v = int(v)
        out += [f"--{k}", str(v)]
    return out


# --------------------------------------------------------------------------------------------
# Scenario run
# --------------------------------------------------------------------------------------------


def run_scenario(sc, bindir, outdir, comp_ms):
    os.makedirs(outdir, exist_ok=True)
    topology = sc["topology"]
    build_topology(topology)
    model = LinkModel(sc, comp_ms)
    model.setup()

    clock = Clock()
    evlog = open(os.path.join(outdir, "netsim.log"), "w", encoding="utf-8")
    ev_lock = threading.Lock()

    def ev(name, **kv):
        with ev_lock:
            extra = "".join(f",{k}={v}" for k, v in kv.items())
            t = clock.now()
            evlog.write(f"[{t:10.6f}] EVENT,netsim,t_ms={int(t * 1000)},ev={name}{extra}\n")
            evlog.flush()

    ev("setup", topology=topology, scenario=sc["name"], comp_ms=f"{comp_ms:.3f}",
       air_overhead_bytes=airtime_overhead_bytes(sc["medium"]))

    procs = []
    pcap_path = os.path.join("/tmp", f"sim_{sc['name']}.pcap")
    b_args = {"report-interval-ms": 1000}
    b_args.update(sc.get("node_b", {}))
    procs.append(Proc("node_b", NS_B, [os.path.join(bindir, "node_b_host")] + kv_args(b_args),
                      os.path.join(outdir, "node_b.log"), clock))
    if topology == "relay":
        procs.append(Proc("relay", NS_R, [os.path.join(bindir, "relay_host"),
                                          "--report-interval-ms", "1000"],
                          os.path.join(outdir, "relay.log"), clock))
    tcpdump = Proc("tcpdump", NS_A, ["tcpdump", "-i", "eth0", "-n", "-U", "-s", "96", "-B", "8192",
                                     "-w", pcap_path, "udp", "port", "5000"],
                   os.path.join(outdir, "tcpdump.log"), clock)
    time.sleep(1.0)

    sweep = sc.get("sweep")
    if sweep is not None:
        # Each step also waits for late echoes (udp-drain-ms) and for node_a_host to start and
        # exit. The deadline is only a backstop: ping is stopped with SIGINT after the last step.
        drain_ms = sweep.get("fixed", {}).get("udp-drain-ms",
                                              sc.get("node_a", {}).get("udp-drain-ms", 2000))
        step_total_s = sweep["step_s"] + drain_ms / 1000.0 + sweep.get("gap_s", 2) + 1
        duration_s = len(sweep["values"]) * step_total_s + 30
    else:
        duration_s = sc["duration_s"]
    ping_cfg = sc.get("ping", {})
    pings = []
    targets = {"node_b": IP_B, "relay": IP_R}
    for tgt in ping_cfg.get("targets", ["node_b"]):
        if tgt == "relay" and topology != "relay":
            continue
        argv = ["ping", "-n", "-O", "-i", f"{ping_cfg.get('interval_ms', 200) / 1000.0:g}",
                "-s", str(ping_cfg.get("size", 56)), "-W", "2", "-w", str(int(duration_s)),
                targets[tgt]]
        pings.append(Proc(f"ping_{tgt}", NS_A, argv, os.path.join(outdir, f"ping_{tgt}.log"), clock))

    # Timed link events.
    stop_events = threading.Event()

    def event_runner():
        todo = []
        for e in sc.get("events", []):
            todo.append((e["at_s"], "begin", e))
            if "duration_s" in e:
                todo.append((e["at_s"] + e["duration_s"], "end", e))
        todo.sort(key=lambda x: x[0])
        for at, phase, e in todo:
            while clock.now() < at:
                if stop_events.wait(min(0.01, max(0.0, at - clock.now()))):
                    return
            hop = e["hop"]
            act = e["action"]
            if act == "link_down":
                if phase == "begin":
                    model.set_hop(hop, loss_pct=100.0, loss_corr_pct=0.0)
                    ev("link_down", hop=hop)
                else:
                    model.restore_hop(hop)
                    ev("link_up", hop=hop)
            elif act == "set":
                if phase == "begin":
                    model.set_hop(hop, **e["params"])
                    ev("hop_set", hop=hop, **{k: v for k, v in e["params"].items()})
                else:
                    model.restore_hop(hop)
                    ev("hop_restore", hop=hop)
            else:
                raise ValueError(f"unknown event action {act!r}")

    evt = threading.Thread(target=event_runner, daemon=True)
    evt.start()

    a_bin = os.path.join(bindir, "node_a_host")
    a_base = {"report-interval-ms": 1000, "relay-echo": int(topology == "relay")}
    a_base.update(sc.get("node_a", {}))
    if topology != "relay":
        a_base["relay-echo"] = 0

    if sweep is None:
        args = dict(a_base)
        args["duration-ms"] = int(duration_s * 1000)
        pa = Proc("node_a", NS_A, [a_bin] + kv_args(args), os.path.join(outdir, "node_a.log"), clock)
        pa.wait(timeout=duration_s + 30)
    else:
        logpath = os.path.join(outdir, "node_a.log")
        for i, value in enumerate(sweep["values"]):
            args = dict(a_base)
            args.update(sweep.get("fixed", {}))
            args[sweep["param"]] = value
            args["duration-ms"] = int(sweep["step_s"] * 1000)
            args["udp-flow"] = f"{sweep.get('flow_prefix', 'sweep')}_{int(value):04d}"
            ev("sweep_step", param=sweep["param"], value=value, flow=args["udp-flow"])
            pa = Proc("node_a", NS_A, [a_bin] + kv_args(args), logpath, clock, append=i > 0)
            pa.wait(timeout=sweep["step_s"] + 30)
            time.sleep(sweep.get("gap_s", 2))

    for p in pings:
        p.signal(signal.SIGINT)
        p.wait(timeout=5)
    stop_events.set()
    evt.join(timeout=2)
    time.sleep(0.5)
    air = model.air_counters()
    gdrop = model.group_drop_count()
    if gdrop is not None:
        gdrop_run = gdrop - (model.group_selftest or 0)
    else:
        gdrop_run = None
    ev("end", air_sent_pkts=air.get("sent_pkts"), air_dropped=air.get("dropped"),
       root_group_tx_dropped=gdrop_run)
    for p in procs:
        p.signal(signal.SIGTERM)
    for p in procs:
        p.wait(timeout=10)
    tcpdump.signal(signal.SIGINT)
    tcpdump.wait(timeout=5)
    evlog.close()

    with open(os.path.join(outdir, "tc_stats.txt"), "w", encoding="utf-8") as f:
        for k, v in model.stats().items():
            f.write(f"## {k}\n{v}\n")

    meta = {
        "scenario": sc,
        "clock": {"t0_mono": clock.t0_mono, "t0_real": clock.t0_real},
        "timer_comp_ms": comp_ms,
        "air_overhead_bytes": airtime_overhead_bytes(sc["medium"]),
        "air_counters": air,
        "root_group_tx_dropped": gdrop_run,
        "root_group_selftest_dropped": model.group_selftest,
        "pcap": pcap_path,
        "kernel": os.uname().release,
    }
    with open(os.path.join(outdir, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)
    teardown()
    return meta


# --------------------------------------------------------------------------------------------
# Calibration of the netem timer overhead
# --------------------------------------------------------------------------------------------


def calibrate(out):
    """Measure how much one netem delay stage adds on top of its configured delay.

    Docker Desktop's VM fires hrtimers late by a fraction of a millisecond, so each netem stage
    adds a roughly constant extra delay. Hop delays are reduced by this amount.
    """
    teardown()
    _ns_base(NS_A)
    _ns_base(NS_B)
    sh(f"ip link add eth0 netns {NS_A} type veth peer name eth0 netns {NS_B}")
    for ns, ip in ((NS_A, "10.99.0.1"), (NS_B, "10.99.0.2")):
        nsx(ns, "ip link set eth0 up")
        nsx(ns, f"ip addr add {ip}/24 dev eth0")

    def ping_avg():
        res = nsx(NS_A, "ping -n -q -c 50 -i 0.02 10.99.0.2")
        line = [ln for ln in res.stdout.splitlines() if "min/avg/max" in ln][0]
        return float(line.split("=")[1].split("/")[1])

    base = ping_avg()
    nsx(NS_A, f"tc qdisc add dev eth0 root netem delay 2ms rate {HOP_FIFO_RATE}")
    with_delay = ping_avg()
    teardown()
    comp = max(0.0, with_delay - base - 2.0)
    result = {"base_rtt_ms": base, "rtt_with_2ms_stage_ms": with_delay, "stage_extra_ms": comp}
    with open(out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
    log(f"calibration: one netem stage adds {comp:.3f} ms (base RTT {base:.3f} ms)")
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("calibrate")
    c.add_argument("--out", required=True)
    r = sub.add_parser("run")
    r.add_argument("--scenario", required=True)
    r.add_argument("--bin", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--calibration")
    sub.add_parser("teardown")
    a = ap.parse_args()

    if os.geteuid() != 0:
        sys.exit("netsim: must run as root inside the halow-hostsim container (docker run --privileged)")

    if a.cmd == "calibrate":
        calibrate(a.out)
    elif a.cmd == "teardown":
        teardown()
    else:
        with open(a.scenario, encoding="utf-8") as f:
            sc = json.load(f)
        comp = 0.0
        if a.calibration and os.path.exists(a.calibration):
            with open(a.calibration, encoding="utf-8") as f:
                comp = json.load(f).get("stage_extra_ms", 0.0)
        log(f"running {sc['name']} ({sc['topology']})")
        try:
            run_scenario(sc, a.bin, a.out, comp)
        finally:
            teardown()


if __name__ == "__main__":
    main()
