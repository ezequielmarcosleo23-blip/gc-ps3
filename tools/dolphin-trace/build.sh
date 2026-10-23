#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compila Dolphin sin interfaz y con el parche de traza (traza.patch), para
# comparar el intérprete de gc-ps3 instrucción por instrucción
# (docs/traza-dolphin.md). Solo hace falta para regenerar las referencias de
# tests/dol/dolphin; make test no lo necesita.
#
#   tools/dolphin-trace/build.sh [carpeta]     (por omisión build/dolphin)
#
# Deja build/dolphin/build/Binaries/dolphin-emu-nogui. Necesita git, cmake,
# ninja, g++ 11 o más nuevo y red hacia github.com. Con 2 núcleos tarda cerca
# de una hora; las demás veces solo recompila lo que cambió.
set -eu

COMMIT=cf5cb7ef81c3e8e7af5c9d566334219ffda9f68b   # el mismo de las semánticas portadas
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:-build/dolphin}
SRC=$DIR/src
OUT=$DIR/build

# Los submódulos que usa esta configuración (el resto queda apagado)
SUBMODULES="Externals/fmt/fmt Externals/imgui/imgui Externals/implot/implot
  Externals/glslang/glslang Externals/tinygltf/tinygltf Externals/enet/enet
  Externals/zlib-ng/zlib-ng Externals/minizip-ng/minizip-ng Externals/SFML/SFML
  Externals/cpp-optparse/cpp-optparse Externals/watcher/watcher Externals/cpp-ipc/cpp-ipc"

if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$DIR"
    git clone --filter=blob:none --no-checkout https://github.com/dolphin-emu/dolphin.git "$SRC"
fi
cd "$SRC"
if [ "$(git rev-parse HEAD 2>/dev/null)" != "$COMMIT" ]; then
    git checkout -q "$COMMIT"
fi
# shellcheck disable=SC2086
git submodule update --init --depth 1 --jobs 4 $SUBMODULES
# El parche: se aplica una sola vez (git apply --check falla si ya está)
if git apply --check "$HERE/traza.patch" 2>/dev/null; then
    git apply "$HERE/traza.patch"
elif ! git apply --reverse --check "$HERE/traza.patch" 2>/dev/null; then
    echo "traza.patch no se aplica ni está aplicado: revisar $SRC" >&2
    exit 1
fi
cd - >/dev/null

# Bibliotecas del sistema que acortan la compilación (Ubuntu 24.04); las que
# faltan se compilan desde Externals.
#   sudo apt install libzstd-dev liblz4-dev libxxhash-dev libbz2-dev liblzo2-dev \
#     libpugixml-dev libmbedtls-dev libcurl4-openssl-dev libhidapi-dev \
#     libusb-1.0-0-dev libspng-dev liblzma-dev pkg-config ccache
cmake -G Ninja -S "$SRC" -B "$OUT" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_QT=OFF -DENABLE_NOGUI=ON -DENABLE_X11=OFF -DENABLE_EGL=OFF \
    -DENABLE_VULKAN=OFF -DENABLE_LLVM=OFF -DENABLE_TESTS=OFF -DENABLE_CUBEB=OFF \
    -DENABLE_ALSA=OFF -DENABLE_PULSEAUDIO=OFF -DUSE_DISCORD_PRESENCE=OFF -DUSE_MGBA=OFF \
    -DENABLE_AUTOUPDATE=OFF -DUSE_RETRO_ACHIEVEMENTS=OFF -DENABLE_ANALYTICS=OFF \
    -DENCODE_FRAMEDUMPS=OFF -DENABLE_SDL=OFF -DENABLE_EVDEV=OFF -DENABLE_HWDB=OFF \
    -DUSE_UPNP=OFF -DENABLE_CLI_TOOL=OFF -DENABLE_CCACHE=ON
ninja -C "$OUT" dolphin-nogui
echo "listo: $OUT/Binaries/dolphin-emu-nogui"
