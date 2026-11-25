#!/usr/bin/env python3
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
"""Unit tests for parse_logs.py on the line formats the firmware prints (standard library only).

Run: python3 -m unittest discover -s sim/tools -p 'test_*.py'
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import parse_logs  # noqa: E402

HIST0 = "/".join(["0"] * 12)


def stats(flow, t_ms, sent, rcvd, lost, rtt_n, rmin, ravg, rmax, hist, extra, final):
    return (f"STATS,{flow},t_ms={t_ms},sent={sent},rcvd={rcvd},lost={lost},dup=0,reord=0,"
            f"stale=0,loss_ppm=0,rtt_n={rtt_n},rtt_min_us={rmin},rtt_avg_us={ravg},"
            f"rtt_max_us={rmax},jitter_us=1000,hist={hist}{extra},final={final}")


UDP_EXTRA = ",tx_err=0,bad=0,foreign=0,corrupt=0"


def tcp_extra(connects):
    return (f",up=1,connects={connects},disconnects=0,stalls=0,connect_fail=0,inflight=0,"
            f"bad_status=0,proto_err=0,crc_err=0,resyncs=0")


class ParseCase(unittest.TestCase):
    def analyse(self, lines, name="node_a"):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, name + ".log")
            with open(path, "w", encoding="utf-8") as f:
                f.write("\n".join(lines) + "\n")
            rec = parse_logs.Records()
            rec.feed_file(path)
            return rec, parse_logs.analyse(rec)


class TestNodeAPing(ParseCase):
    LINES = [
        "I (5400) node_a: ESP log line in between",
        "PING,node_b,t_ms=5410,ev=reply,dst=192.168.50.1,seq=1,rtt_ms=21,ttl=64,size=64",
        "PING,node_b,t_ms=6410,ev=reply,dst=192.168.50.1,seq=2,rtt_ms=25,ttl=64,size=64",
        "PING,node_b,t_ms=8410,ev=timeout,dst=192.168.50.1,seq=3",
        "PING,node_b,t_ms=8420,ev=summary,dst=192.168.50.1,sent=3,rcvd=2,lost=1,loss_ppm=333333,"
        "rtt_min_ms=21,rtt_avg_ms=23,rtt_max_ms=25,running=1",
        "PING,relay,t_ms=5395,ev=reply,dst=192.168.50.2,seq=1,rtt_ms=9,ttl=255,size=64",
    ]

    def test_replies_timeouts_and_summary(self):
        rec, summary = self.analyse(self.LINES)
        b = summary["flows"]["ping_node_b"]
        self.assertEqual((b["tx"], b["rx"], b["lost"]), (3, 2, 1))
        self.assertAlmostEqual(b["loss_pct"], 33.333)
        self.assertEqual((b["rtt_min_ms"], b["rtt_max_ms"]), (21.0, 25.0))
        # The summary line is a total, not a sample.
        self.assertEqual(len([p for p in rec.pings if p["flow"] == "ping_node_b"]), 3)
        relay = summary["flows"]["ping_relay"]
        self.assertEqual((relay["tx"], relay["rx"], relay["rtt_min_ms"]), (1, 1, 9.0))
        self.assertNotIn("node_b", summary["flows"])

    def test_summary_only(self):
        lines = [
            "PING,node_b,t_ms=10300,ev=summary,dst=192.168.50.1,sent=5,rcvd=5,lost=0,"
            "loss_ppm=0,rtt_min_ms=18,rtt_avg_ms=22,rtt_max_ms=31,running=1",
            "PING,node_b,t_ms=15300,ev=summary,dst=192.168.50.1,sent=10,rcvd=9,lost=1,"
            "loss_ppm=100000,rtt_min_ms=18,rtt_avg_ms=24,rtt_max_ms=40,running=1",
        ]
        _, summary = self.analyse(lines)
        b = summary["flows"]["ping_node_b"]
        self.assertEqual((b["tx"], b["rx"], b["loss_pct"]), (10, 9, 10.0))
        self.assertEqual((b["rtt_min_ms"], b["rtt_avg_ms"], b["rtt_max_ms"]), (18, 24, 40))
        self.assertEqual(b["dst"], "192.168.50.1")

    def test_log_cut_after_summary(self):
        # Replies after the last summary are not in its "sent" yet; loss must not go negative.
        lines = self.LINES[1:5] + [
            "PING,node_b,t_ms=9410,ev=reply,dst=192.168.50.1,seq=4,rtt_ms=22,ttl=64,size=64",
            "PING,node_b,t_ms=10410,ev=reply,dst=192.168.50.1,seq=5,rtt_ms=23,ttl=64,size=64",
        ]
        _, summary = self.analyse(lines)
        b = summary["flows"]["ping_node_b"]
        self.assertEqual((b["tx"], b["rx"], b["lost"]), (5, 4, 1))

    def test_probe_restart(self):
        lines = [
            "PING,node_b,t_ms=1000,ev=reply,dst=192.168.50.1,seq=1,rtt_ms=20,ttl=64,size=64",
            "PING,node_b,t_ms=2000,ev=reply,dst=192.168.50.1,seq=2,rtt_ms=20,ttl=64,size=64",
            "PING,node_b,t_ms=9000,ev=reply,dst=192.168.50.1,seq=1,rtt_ms=30,ttl=64,size=64",
            "PING,node_b,t_ms=10000,ev=timeout,dst=192.168.50.1,seq=2",
        ]
        _, summary = self.analyse(lines)
        b = summary["flows"]["ping_node_b"]
        self.assertEqual((b["tx"], b["rx"]), (4, 3))


class TestClientRestarts(ParseCase):
    def test_runs_are_aggregated(self):
        h1 = "0/0/0/0/0/10/0/0/0/0/0/0"
        h2 = "0/0/0/0/0/0/5/0/0/0/0/0"
        lines = [
            "EVENT,udp,t_ms=0,ev=start,dst=192.168.50.1:5000,rate_pps=10,payload=64,count=0,"
            "duration_ms=0,session=aaaaaaaa",
            "EVENT,tcp,t_ms=0,ev=start,dst=192.168.50.1:5001,rate_mps=5,payload=256,count=0,"
            "duration_ms=0,session=bbbbbbbb",
            "EVENT,tcp,t_ms=2,ev=connected,attempt=1,connect_ms=35,outage_ms=0,next_seq=0",
            stats("udp", 2000, 20, 10, 10, 10, 20000, 25000, 30000, h1, UDP_EXTRA, 0),
            stats("tcp", 2000, 10, 10, 0, 10, 20000, 25000, 30000, h1, tcp_extra(1), 0),
            "EVENT,svc,t_ms=3000,ev=stop,reason=link_down",
            stats("udp", 2100, 20, 10, 10, 10, 20000, 25000, 30000, h1, UDP_EXTRA, 1),
            stats("tcp", 2100, 10, 10, 0, 10, 20000, 25000, 30000, h1, tcp_extra(1), 1),
            "EVENT,udp,t_ms=0,ev=start,dst=192.168.50.1:5000,rate_pps=10,payload=64,count=0,"
            "duration_ms=0,session=cccccccc",
            "EVENT,tcp,t_ms=0,ev=start,dst=192.168.50.1:5001,rate_mps=5,payload=256,count=0,"
            "duration_ms=0,session=dddddddd",
            "EVENT,tcp,t_ms=3,ev=connected,attempt=1,connect_ms=40,outage_ms=0,next_seq=0",
            stats("udp", 1000, 10, 10, 0, 5, 40000, 50000, 60000, h2, UDP_EXTRA, 1),
            stats("tcp", 1000, 5, 5, 0, 5, 40000, 50000, 60000, h2, tcp_extra(1), 1),
        ]
        _, summary = self.analyse(lines)
        udp = summary["flows"]["udp"]
        self.assertEqual((udp["sent"], udp["rcvd"], udp["lost"]), (30, 20, 10))
        self.assertAlmostEqual(udp["loss_pct"], 33.333)
        self.assertEqual((udp["runs"], udp["restarts"]), (2, 1))
        self.assertEqual(udp["sessions"], ["aaaaaaaa", "cccccccc"])
        self.assertEqual((udp["rtt_min_ms"], udp["rtt_max_ms"]), (20.0, 60.0))
        self.assertAlmostEqual(udp["rtt_avg_ms"], (25.0 * 10 + 50.0 * 5) / 15, places=3)
        self.assertEqual(udp["hist"][5:7], [10, 5])

        tcp = summary["flows"]["tcp"]
        self.assertEqual((tcp["sent"], tcp["rcvd"], tcp["connects"]), (15, 15, 2))
        self.assertEqual(tcp["reconnects"], 1)
        self.assertEqual(len([e for e in tcp["connection_events"] if e["ev"] == "connected"]), 2)

        # Each run keeps its own start parameters.
        segs = [s for s in summary["segments"] if s["flow"] == "udp"]
        self.assertEqual([s["session"] for s in segs], ["aaaaaaaa", "cccccccc"])
        tsegs = [s for s in summary["segments"] if s["flow"] == "tcp"]
        self.assertEqual([s["reconnects"] for s in tsegs], [0, 0])

    def test_single_run_unchanged(self):
        lines = [
            "EVENT,udp,t_ms=0,ev=start,dst=192.168.50.1:5000,rate_pps=10,payload=64,count=0,"
            "duration_ms=0,session=aaaaaaaa",
            stats("udp", 1000, 10, 10, 0, 10, 20000, 25000, 30000, HIST0, UDP_EXTRA, 1),
        ]
        _, summary = self.analyse(lines)
        udp = summary["flows"]["udp"]
        self.assertNotIn("runs", udp)
        self.assertEqual(udp["sent"], 10)
        self.assertNotIn("_first_line", udp)


if __name__ == "__main__":
    unittest.main()
