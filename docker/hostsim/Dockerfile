# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Host-side environment for the lab: builds and tests common/ (unit tests, sanitizers, loopback
# integration test) and runs the network-namespace simulation (iproute2/tc netem, ping, tcpdump)
# and the Python log/plot tools. Nothing is installed on the host.
#
#   docker build -t halow-hostsim:1 docker/hostsim
#   docker run --rm -v "$PWD":/work halow-hostsim:1 \
#       sh -c 'cmake -S common -B /tmp/b -DLAB_SANITIZE=ON && cmake --build /tmp/b -j && \
#              ctest --test-dir /tmp/b --output-on-failure'
#
# The netns simulation needs: --cap-add NET_ADMIN (or --privileged) on the docker run.
FROM debian:bookworm-slim

RUN apt-get update && DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        iproute2 \
        iputils-ping \
        procps \
        python3 \
        python3-matplotlib \
        python3-venv \
        tcpdump \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["/bin/bash"]
