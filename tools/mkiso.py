#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Archivo de prueba de lectura para la prueba "iso" de la Fase 0.

Cada palabra de 32 bits (big-endian) vale su posición en bytes, el mismo patrón
que src/core/pattern.c: la consola verifica cada byte leído sin copia de
referencia. Por omisión tiene el tamaño de un disco de GameCube (1 459 978 240
bytes); no contiene nada de Nintendo.

    python3 tools/mkiso.py patron.bin                 # genera 1.4 GB
    python3 tools/mkiso.py patron.bin --size 64M      # otro tamaño (K, M, G)
    python3 tools/mkiso.py --verify patron.bin        # comprueba un archivo
    python3 tools/mkiso.py --selftest                 # prueba interna (la corre `make test`)

Dónde ponerlo: en la consola /dev_hdd0/gcps3/patron.bin (y opcional
/dev_usb000/gcps3/patron.bin); en RPCS3, la carpeta dev_hdd0/gcps3/ dentro de
la de RPCS3. No en dev_hdd0/tmp: RPCS3 la vacía al arrancar cada programa.
"""
import argparse
import array
import os
import sys
import tempfile
import time

GC_DISC_SIZE = 1459978240        # 712 880 sectores de 2048 bytes
CHUNK = 4 << 20                  # 4 MB por escritura
FIXTURE = (0x12345678, bytes([0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x7C]))   # = tests/unit/test_pattern.c


def pattern(offset, length):
    """Bytes del patrón en [offset, offset + length); ambos múltiplos de 4."""
    if offset % 4 or length % 4:
        raise ValueError("offset y longitud deben ser múltiplos de 4")
    out = bytearray()
    start = offset & 0xFFFFFFFF
    words = length // 4
    while words:
        n = min(words, (0x100000000 - start) // 4)       # hasta dar la vuelta en 4 GB
        a = array.array("I", range(start, start + 4 * n, 4))
        if sys.byteorder == "little":
            a.byteswap()
        out += a.tobytes()
        words -= n
        start = 0
    return bytes(out)


def parse_size(text):
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    text = text.strip().upper()
    if text[-1:] in mult:
        return int(float(text[:-1]) * mult[text[-1]])
    return int(text)


def make(path, size):
    if size % 4:
        raise SystemExit("el tamaño debe ser múltiplo de 4")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    t0 = time.monotonic()
    with open(path, "wb") as f:
        for off in range(0, size, CHUNK):
            f.write(pattern(off, min(CHUNK, size - off)))
            if off % (256 << 20) == 0 and off:
                print(f"  {off >> 20} MB...", flush=True)
    dt = time.monotonic() - t0
    print(f"{path}: {size} bytes en {dt:.1f} s ({size / (1 << 20) / max(dt, 1e-6):.0f} MB/s)")


def verify(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        off = 0
        while off < size:
            data = f.read(CHUNK)
            n = len(data) - len(data) % 4
            exp = pattern(off, n)
            if data[:n] != exp:
                bad = next(i for i in range(0, n, 4) if data[i:i + 4] != exp[i:i + 4])
                print(f"{path}: DISTINTO en el byte {off + bad} (0x{off + bad:x})")
                return 1
            off += len(data)
    print(f"{path}: OK, {size} bytes con el patrón")
    return 0


def selftest():
    def check(cond, what):
        if not cond:
            raise SystemExit(f"mkiso selftest: FALLO: {what}")

    check(pattern(FIXTURE[0], 8) == FIXTURE[1], "bytes de referencia")
    check(pattern(0xFFFFFFF8, 16) == bytes.fromhex("fffffff8fffffffc0000000000000004"), "vuelta en 4 GB")
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "patron.bin")
        make(p, CHUNK + 4096)
        check(os.path.getsize(p) == CHUNK + 4096, "tamaño")
        check(verify(p) == 0, "verificación del archivo generado")
        with open(p, "r+b") as f:
            f.seek(CHUNK + 100)
            f.write(b"\xff")
        check(verify(p) == 1, "detecta un byte cambiado")
    print("mkiso selftest: OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Archivo de prueba de lectura con patrón de posición")
    ap.add_argument("path", nargs="?")
    ap.add_argument("--size", default=str(GC_DISC_SIZE), help="tamaño en bytes (o con K, M, G)")
    ap.add_argument("--verify", action="store_true", help="comprobar en lugar de generar")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.path:
        ap.error("falta la ruta del archivo")
    if args.verify:
        return verify(args.path)
    make(args.path, parse_size(args.size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
