#!/usr/bin/env bash
#
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Build the EKH05 relay firmware inside the ekh05-builder Docker image (nothing is installed on
# the host). Applies the mm-iot-sdk patches to third_party/mm-iot-sdk first.
#
# Usage: relay_ekh05/build.sh [--no-patches] [--revert-patches] [--image NAME] [-j N]
#                             [make targets / VAR=value ...]
#
#   --no-patches      build the SDK tree as it is (only reports the patch state; sets
#                     ALLOW_UNPATCHED=1 so the Makefile's patch check does not stop the link)
#   --revert-patches  revert the SDK patches after the build, leaving the submodule pristine
#   --image NAME      Docker image (default ekh05-builder:2.14.3, or $EKH05_IMAGE)
#   -j N              parallel jobs (default: number of host CPUs)
#
# Default make goal: "images" (elf, bin, hex and a size report) in relay_ekh05/build.
# Examples:
#   relay_ekh05/build.sh
#   relay_ekh05/build.sh --no-patches BUILD_DIR=build_nopatch
#   relay_ekh05/build.sh bin-with-config
#   relay_ekh05/build.sh clean

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
APP_REL="${SCRIPT_DIR#"${REPO_ROOT}"/}"

image="${EKH05_IMAGE:-ekh05-builder:2.14.3}"
patches=1
revert=0
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
make_args=()

while [ $# -gt 0 ]; do
    case "$1" in
        --no-patches) patches=0 ;;
        --revert-patches) revert=1 ;;
        --image)
            shift
            image="$1"
            ;;
        -j)
            shift
            jobs="$1"
            ;;
        -j*) jobs="${1#-j}" ;;
        -h | --help)
            sed -n '7,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) make_args+=("$1") ;;
    esac
    shift
done

if [ ${#make_args[@]} -eq 0 ]; then
    make_args=(images)
fi

command -v docker >/dev/null 2>&1 || {
    echo "error: docker not found" >&2
    exit 1
}
docker image inspect "${image}" >/dev/null 2>&1 || {
    echo "error: Docker image ${image} not found; build it with:" >&2
    echo "  docker build -t ${image} ${REPO_ROOT}/docker/ekh05-builder" >&2
    exit 1
}

# The script runs inside the container with the repository mounted at /work. git in the
# container runs as root on a host-owned checkout, so mark it safe for the patch tool.
inner='set -euo pipefail
cd /work
if [ "$PATCHES" = 1 ]; then
    tools/apply_sdk_patches.sh third_party/mm-iot-sdk
else
    echo "== --no-patches: SDK patch state (not changed)"
    tools/apply_sdk_patches.sh --check third_party/mm-iot-sdk || true
    export ALLOW_UNPATCHED=1
fi
rc=0
make -C "$APP_REL" -j"$JOBS" "$@" || rc=$?
if [ "$REVERT" = 1 ]; then
    tools/apply_sdk_patches.sh --revert third_party/mm-iot-sdk
fi
exit $rc'

tty_args=()
if [ -t 1 ]; then
    tty_args=(-t)
fi

exec docker run --rm ${tty_args[@]+"${tty_args[@]}"} \
    -v "${REPO_ROOT}":/work \
    -e PATCHES="${patches}" -e REVERT="${revert}" -e JOBS="${jobs}" -e APP_REL="${APP_REL}" \
    -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
    -e PYTHONDONTWRITEBYTECODE=1 \
    "${image}" bash -c "${inner}" bash "${make_args[@]}"
