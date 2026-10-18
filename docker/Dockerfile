# SPDX-License-Identifier: GPL-2.0-or-later
#
# Imagen de build de gc-ps3: la misma en la PC (make docker-make) y en GitHub Actions.
# Solo descarga el nightly precompilado de ps3dev fijado en tools/toolchain.env
# (ps3dev-docker compila todo desde fuente y es demasiado lento para CI).
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl make gcc libc6-dev \
        libelf1t64 zlib1g libgmp10 libssl3t64 libzstd1 \
    && rm -rf /var/lib/apt/lists/*

COPY tools/toolchain.env tools/fetch-toolchain.sh /tmp/tools/
RUN sh /tmp/tools/fetch-toolchain.sh /usr/local/ps3dev && rm -rf /tmp/tools

ENV PS3DEV=/usr/local/ps3dev \
    PSL1GHT=/usr/local/ps3dev \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/ps3dev/bin:/usr/local/ps3dev/ppu/bin:/usr/local/ps3dev/spu/bin

WORKDIR /src
