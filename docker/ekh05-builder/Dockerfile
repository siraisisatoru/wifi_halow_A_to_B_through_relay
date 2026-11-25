# Build environment for the MM6108-EKH05 relay firmware (mm-iot-sdk make flow).
# Mirrors mm-iot-sdk 2.14.3 framework/tools/setupscripts (Ubuntu 22.04, xPack GCC 14.2.1-1.1,
# xPack OpenOCD 0.12.0-2, pipenv deps from framework/Pipfile) so nothing is installed on the host.
#
#   docker build -t ekh05-builder:2.14.3 docker/ekh05-builder
#   docker run --rm -v "$PWD":/work ekh05-builder:2.14.3 make -C relay_ekh05
FROM ubuntu:22.04

ARG TOOLCHAIN_VERSION=14.2.1-1.1
ARG OPENOCD_VERSION=0.12.0-2
ARG TARGETARCH

RUN apt-get update && DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --no-install-recommends \
        build-essential ca-certificates dos2unix git libncurses5 make \
        python3 python-is-python3 python3-pip python3-serial wget xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN case "${TARGETARCH:-$(dpkg --print-architecture)}" in \
        amd64) XARCH=x64 ;; \
        arm64) XARCH=arm64 ;; \
        *) echo "unsupported arch" && exit 1 ;; \
    esac \
    && mkdir -p /opt/morse && cd /tmp \
    && GCC_TAR=xpack-arm-none-eabi-gcc-${TOOLCHAIN_VERSION}-linux-${XARCH}.tar.gz \
    && wget -q "https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v${TOOLCHAIN_VERSION}/${GCC_TAR}" \
    && wget -q "https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v${TOOLCHAIN_VERSION}/${GCC_TAR}.sha" \
    && sha256sum -c "${GCC_TAR}.sha" \
    && tar -xf "${GCC_TAR}" -C /opt/morse \
    && OCD_TAR=xpack-openocd-${OPENOCD_VERSION}-linux-${XARCH}.tar.gz \
    && wget -q "https://github.com/xpack-dev-tools/openocd-xpack/releases/download/v${OPENOCD_VERSION}/${OCD_TAR}" \
    && wget -q "https://github.com/xpack-dev-tools/openocd-xpack/releases/download/v${OPENOCD_VERSION}/${OCD_TAR}.sha" \
    && sha256sum -c "${OCD_TAR}.sha" \
    && tar -xf "${OCD_TAR}" -C /opt/morse \
    && rm -f /tmp/*.tar.gz /tmp/*.sha

# Python packages the SDK build/config-store tools need (framework/Pipfile [packages]).
RUN pip3 install --no-cache-dir "protobuf~=3.20.3" coloredlogs hjson pyelftools dacite pyserial

ENV PATH=/opt/morse/xpack-arm-none-eabi-gcc-${TOOLCHAIN_VERSION}/bin:/opt/morse/xpack-openocd-${OPENOCD_VERSION}/bin:${PATH}
WORKDIR /work
CMD ["/bin/bash"]
