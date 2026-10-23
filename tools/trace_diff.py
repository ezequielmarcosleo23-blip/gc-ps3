#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# gc-ps3 - emulador de GameCube para PlayStation 3
# Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
"""Compara dos huellas de instrucciones (runner --huella o el Dolphin parchado).

Cada huella tiene 16 bytes por instrucción (pc, instrucción y huella del estado
de la CPU antes de ejecutarla, little-endian; docs/traza-dolphin.md). Dice
hasta dónde son iguales y, en la primera diferencia, muestra las instrucciones
de antes. Si están los .detalle de las dos con esa instrucción, dice qué
registros son distintos.

    python3 tools/trace_diff.py hola.runner.huella hola.dolphin.huella
    python3 tools/trace_diff.py --selftest
"""
import argparse
import os
import struct
import sys
import tempfile

REC = 16
PALABRAS = 89
DET = 16 + 8 * PALABRAS
CHUNK = REC * 65536


def records(path, start, count):
    """Registros [start, start + count) como (pc, inst, estado)."""
    with open(path, "rb") as f:
        f.seek(start * REC)
        data = f.read(count * REC)
    return [struct.unpack_from("<IIQ", data, k) for k in range(0, len(data) - REC + 1, REC)]


def first_difference(a, b):
    """(índice de la primera instrucción distinta o None, largo de a, largo de b)."""
    na, nb = os.path.getsize(a) // REC, os.path.getsize(b) // REC
    with open(a, "rb") as fa, open(b, "rb") as fb:
        base = 0
        while True:
            x, y = fa.read(CHUNK), fb.read(CHUNK)
            if not x or not y:
                break
            if x != y:
                n = min(len(x), len(y))
                for k in range(0, n, REC):
                    if x[k:k + REC] != y[k:k + REC]:
                        return base + k // REC, na, nb
                if len(x) != len(y):
                    break
            base += len(x) // REC
    return (None if na == nb else min(na, nb)), na, nb


def fields(words):
    """Las 89 palabras del estado, con el nombre de cada registro."""
    d = {}
    for k in range(16):
        d[f"r{2 * k}"] = words[k] & 0xFFFFFFFF
        d[f"r{2 * k + 1}"] = words[k] >> 32
    for i in range(32):
        d[f"f{i}.ps0"] = words[16 + 2 * i]
        d[f"f{i}.ps1"] = words[17 + 2 * i]
    pairs = [("cr", "xer"), ("lr", "ctr"), ("msr", "fpscr"), ("srr0", "srr1"),
             ("gqr0", "gqr1"), ("gqr2", "gqr3"), ("gqr4", "gqr5"), ("gqr6", "gqr7"),
             ("hid0", "hid2")]
    for k, (lo, hi) in enumerate(pairs):
        d[lo] = words[80 + k] & 0xFFFFFFFF
        d[hi] = words[80 + k] >> 32
    return d


def detail(path, index):
    """(pc, inst, campos) de la instrucción index en path.detalle, o None."""
    det = path + ".detalle"
    if not os.path.exists(det):
        return None
    with open(det, "rb") as f:
        data = f.read()
    for k in range(0, len(data) - DET + 1, DET):
        n, pc, inst = struct.unpack_from("<QII", data, k)
        if n == index:
            return pc, inst, fields(struct.unpack_from(f"<{PALABRAS}Q", data, k + 16))
    return None


def report(a, b, names=("A", "B"), out=sys.stdout):
    """Imprime la comparación. Devuelve el índice de la primera diferencia o None."""
    idx, na, nb = first_difference(a, b)
    if idx is None:
        print(f"iguales: {na} instrucciones", file=out)
        return None
    ra, rb = records(a, idx, 1), records(b, idx, 1)
    if not ra or not rb:
        longer = names[0] if na > nb else names[1]
        print(f"iguales en las primeras {idx} instrucciones; {longer} sigue "
              f"({na} contra {nb})", file=out)
        return idx
    print(f"distintas en la instrucción {idx} (de {na} y {nb})", file=out)
    for pc, inst, _ in records(a, max(0, idx - 4), min(idx, 4)):
        print(f"   antes       {pc:08x} {inst:08x}", file=out)
    (pa, ia, ea), (pb, ib, eb) = ra[0], rb[0]
    print(f"   {names[0]:<11} {pa:08x} {ia:08x} estado {ea:016x}", file=out)
    print(f"   {names[1]:<11} {pb:08x} {ib:08x} estado {eb:016x}", file=out)
    da, db = detail(a, idx), detail(b, idx)
    if da and db:
        diff = [k for k in da[2] if da[2][k] != db[2][k]]
        print("   registros distintos antes de esa instrucción:", file=out)
        for k in diff:
            width = 16 if "." in k else 8
            print(f"     {k:<8} {names[0]} {da[2][k]:0{width}x}  {names[1]} {db[2][k]:0{width}x}",
                  file=out)
    else:
        print(f"   (para ver los registros: el detalle de las instrucciones {max(0, idx - 4)}:{idx + 1})",
              file=out)
    return idx


def selftest():
    import io
    import random
    rnd = random.Random(1)
    tmp = tempfile.mkdtemp(prefix="trace_diff_")
    recs = [(0x80003100 + 4 * k, rnd.getrandbits(32), rnd.getrandbits(64)) for k in range(70000)]

    def write(name, rs, det=None):
        p = os.path.join(tmp, name)
        with open(p, "wb") as f:
            for r in rs:
                f.write(struct.pack("<IIQ", *r))
        if det is not None:
            with open(p + ".detalle", "wb") as f:
                for n, words in det:
                    f.write(struct.pack("<QII", n, recs[n][0], recs[n][1]))
                    f.write(struct.pack(f"<{PALABRAS}Q", *words))
        return p

    words = [rnd.getrandbits(64) for _ in range(PALABRAS)]
    changed = list(words)
    changed[1] ^= 1 << 32                      # r3
    changed[81] ^= 0x10                        # lr
    other = list(recs)
    other[66000] = (other[66000][0], other[66000][1], other[66000][2] ^ 1)
    a = write("a", recs, [(66000, words)])
    b = write("b", other, [(66000, changed)])
    same = write("c", recs)
    short = write("d", recs[:500])
    sink = io.StringIO()
    checks = [
        ("iguales", report(a, same, out=sink) is None),
        ("diferencia en 66000", report(a, b, out=sink) == 66000),
        ("más corta", report(a, short, out=sink) == 500),
        ("registros", "r3 " in sink.getvalue() and "lr " in sink.getvalue()
         and "r2 " not in sink.getvalue()),
    ]
    bad = [n for n, ok in checks if not ok]
    if bad:
        print(sink.getvalue())
        print("trace_diff selftest: FALLA en " + ", ".join(bad))
        return 1
    print("trace_diff selftest: OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a", nargs="?")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--nombres", default="A,B", help="cómo llamar a cada huella (runner,dolphin)")
    ap.add_argument("--selftest", action="store_true")
    o = ap.parse_args()
    if o.selftest:
        return selftest()
    if not o.a or not o.b:
        ap.error("faltan las dos huellas")
    return 0 if report(o.a, o.b, tuple(o.nombres.split(",", 1))) is None else 1


if __name__ == "__main__":
    sys.exit(main())
