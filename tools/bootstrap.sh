#!/usr/bin/env bash
#
# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Prepare a fresh clone: check out the submodules, apply the mm-iot-sdk patches and, optionally,
# build the Docker images. Safe to run again (after a pull or a submodule update).
#
# Usage: tools/bootstrap.sh [--docker] [--no-patches]
#
#   --docker      also build the ekh05-builder:2.14.3 and halow-hostsim:1 Docker images
#   --no-patches  leave the SDK trees pristine (see patches/README.md for what that costs)
#
# Environment:
#   MM_IOT_SDK_URL  where esp-halow's nested mm-iot-sdk submodule is fetched from
#                   (default https://github.com/MorseMicro/mm-iot-sdk.git)
#
# esp-halow's .gitmodules points its mm-iot-sdk submodule at the relative URL ../mm-iot-sdk.git,
# which only resolves for clones of MorseMicro/esp-halow. The fork used here keeps that URL
# unchanged, so the URL is overridden in the submodule's local git config instead.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ESP_HALOW="third_party/esp-halow"
NESTED_SDK="halow/components/mm-iot-sdk"
SDK_URL="${MM_IOT_SDK_URL:-https://github.com/MorseMicro/mm-iot-sdk.git}"

usage()
{
    sed -n '7,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

docker_images=0
patches=1
while [ $# -gt 0 ]; do
    case "$1" in
        --docker) docker_images=1 ;;
        --no-patches) patches=0 ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option '$1' (see --help)" >&2
            exit 1
            ;;
    esac
    shift
done

cd "${REPO_ROOT}"

echo "== submodules"
git submodule sync -- third_party/mm-iot-sdk "${ESP_HALOW}"
git submodule update --init -- third_party/mm-iot-sdk "${ESP_HALOW}"

git -C "${ESP_HALOW}" config "submodule.${NESTED_SDK}.url" "${SDK_URL}"
git -C "${ESP_HALOW}" config "submodule.${NESTED_SDK}.active" true
git -C "${ESP_HALOW}" submodule update --init
git submodule status --recursive

if [ "${patches}" -eq 1 ]; then
    echo "== mm-iot-sdk patches"
    "${SCRIPT_DIR}/apply_sdk_patches.sh"
fi

if [ "${docker_images}" -eq 1 ]; then
    echo "== Docker images"
    docker build -t ekh05-builder:2.14.3 docker/ekh05-builder
    docker build -t halow-hostsim:1 docker/hostsim
fi

cat <<'EOF'
== done
Next:
  relay:     relay_ekh05/build.sh          (needs the ekh05-builder image: --docker)
  node A/B:  source ~/.espressif/tools/activate_idf_v6.0.2.sh   (bash or zsh)
             cd node_b && idf.py set-target esp32s3 build
  docs:      docs/bringup.md
EOF
