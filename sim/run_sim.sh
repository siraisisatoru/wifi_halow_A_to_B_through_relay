#!/usr/bin/env bash
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Run the A -> relay -> B network simulation. Everything runs inside the halow-hostsim Docker
# image (privileged, for network namespaces and tc); nothing is installed on the host.
#
#   sim/run_sim.sh                      all scenarios in sim/scenarios/
#   sim/run_sim.sh relay_outage ...     only these (file names without .json)
#   sim/run_sim.sh --parse-only [...]   re-parse, re-plot and rewrite SUMMARY.md from existing logs
#
# Environment: SIM_IMAGE (default halow-hostsim:1).
# Output: sim/results/<scenario>/{*.log, stats.csv, events.csv, ping.csv, udp_rtt.csv.gz,
#         summary.json, meta.json, tc_stats.txt, *.png} and sim/results/SUMMARY.md.

set -euo pipefail

IMAGE=${SIM_IMAGE:-halow-hostsim:1}
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)

if [ "${SIM_IN_CONTAINER:-0}" != "1" ]; then
    exec docker run --rm --privileged \
        -e SIM_IN_CONTAINER=1 -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        -v "$REPO":/work -w /work "$IMAGE" bash sim/run_sim.sh "$@"
fi

# ---- inside the container ---------------------------------------------------------------
cd /work
export MPLCONFIGDIR=/tmp/mpl
PARSE_ONLY=0
if [ "${1:-}" = "--parse-only" ]; then
    PARSE_ONLY=1
    shift
fi

SCENARIOS=("$@")
if [ ${#SCENARIOS[@]} -eq 0 ]; then
    for f in sim/scenarios/*.json; do
        SCENARIOS+=("$(basename "$f" .json)")
    done
fi

RESULTS=sim/results
BUILD=/tmp/simbuild
mkdir -p "$RESULTS"

fix_owner()
{
    if [ -n "${HOST_UID:-}" ]; then
        chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" "$RESULTS" 2>/dev/null || true
    fi
}
trap fix_owner EXIT

if [ "$PARSE_ONLY" -eq 0 ]; then
    echo "== build host apps"
    cmake -S sim/host_apps -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
    cmake --build "$BUILD" -j >/dev/null

    echo "== calibrate netem timer overhead"
    python3 sim/tools/netsim.py calibrate --out "$RESULTS/calibration.json"
fi

for sc in "${SCENARIOS[@]}"; do
    out="$RESULTS/$sc"
    if [ "$PARSE_ONLY" -eq 0 ]; then
        echo "== scenario $sc"
        rm -rf "$out"
        python3 sim/tools/netsim.py run --scenario "sim/scenarios/$sc.json" --bin "$BUILD" \
            --out "$out" --calibration "$RESULTS/calibration.json"
    fi
    echo "== parse $sc"
    python3 sim/tools/parse_logs.py --sim-dir "$out" || echo "   ($sc: some checks failed)"
    python3 sim/tools/plot_results.py "$out"
    # Keep committed raw logs small.
    for f in "$out"/*.log; do
        [ -f "$f" ] || continue
        if [ "$(stat -c %s "$f")" -gt 1000000 ]; then
            gzip -9f "$f"
        fi
    done
done

python3 sim/tools/plot_results.py --compare "$RESULTS"
python3 sim/tools/make_summary.py "$RESULTS"
