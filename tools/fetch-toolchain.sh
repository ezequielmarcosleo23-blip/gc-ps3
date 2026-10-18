#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Descarga el nightly de ps3dev fijado en tools/toolchain.env, verifica su SHA-256
# y lo instala en el directorio indicado (por defecto /usr/local/ps3dev).
# Lo usan docker/Dockerfile y el CI; en la PC el toolchain ya lo instaló 02_setup_ubuntu.sh.
set -eu

DEST=${1:-/usr/local/ps3dev}
DIR=$(cd "$(dirname "$0")" && pwd)
. "$DIR/toolchain.env"

URL="https://github.com/ps3dev/ps3dev/releases/download/${PS3DEV_TAG}/ps3dev-linux-X64.tar.gz"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "Descargando $PS3DEV_TAG ..."
curl -fsSL --retry 3 -o "$TMP/ps3dev.tar.gz" "$URL"
echo "$PS3DEV_SHA256  $TMP/ps3dev.tar.gz" | sha256sum -c -

tar -xzf "$TMP/ps3dev.tar.gz" -C "$TMP"
mkdir -p "$DEST"
cp -a "$TMP/ps3dev/." "$DEST/"
echo "$PS3DEV_TAG" > "$DEST/.ps3dev_tag"
echo "Toolchain $PS3DEV_TAG instalado en $DEST"
