#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pérdidas por ráfaga de la prueba udplog de la Fase 0.

Lee el log que guardó el receptor (logs/<fecha>_udplog_<sesión>.log) y, para
cada ráfaga ("r1 17/10000", "h2 5/2500"...), cuenta las líneas que llegaron, las
perdidas y el ritmo con que salieron de la consola (según t_us). Suma también
las cifras que la propia consola informa al terminar cada ráfaga.

    python3 tools/udplog_stats.py                  # el log de udplog más reciente en logs/
    python3 tools/udplog_stats.py logs/archivo.log
    python3 tools/udplog_stats.py --selftest       # prueba interna (la corre `make test`)

Sale con 0 si las ráfagas con ritmo (r2, r3) y los hilos llegaron completos,
1 si no, 2 si el archivo no sirve.
"""
import glob
import os
import re
import sys

sys.dont_write_bytecode = True   # sin __pycache__ en tools/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from logrecv import parse  # noqa: E402  (mismo formato de línea)

ITEM_RE = re.compile(r"^([a-z][a-z0-9]*) (\d+)/(\d+)$")
CONSOLE_RE = re.compile(r"^rafaga (\S+): (\d+) lineas en (\d+) us .*?(\d+) envios fallidos$")
RATES = {"r1": "sin pausa", "r2": "1000/s", "r3": "100/s"}
MUST_BE_COMPLETE = ("r2", "r3", "hilos")   # criterio de la Fase 0


def analyze(lines):
    """Devuelve (ráfagas, consola, totales) a partir de las líneas del archivo."""
    bursts = {}
    console = {}
    seqs = set()
    result = None
    for line in lines:
        rec = parse(line.rstrip("\n"))
        if rec is None:
            continue
        seqs.add(rec["seq"])
        if rec["level"] == "R":
            result = rec["msg"]
        m = CONSOLE_RE.match(rec["msg"])
        if m and rec["level"] in "IW":
            console[m.group(1)] = {"us": int(m.group(3)), "errors": int(m.group(4))}
            continue
        if rec["level"] != "D":
            continue
        m = ITEM_RE.match(rec["msg"])
        if not m:
            continue
        name, i, total = m.group(1), int(m.group(2)), int(m.group(3))
        group = "hilos" if re.fullmatch(r"h\d+", name) else name
        b = bursts.setdefault(group, {"parts": {}, "t0": None, "t1": None})
        p = b["parts"].setdefault(name, {"total": total, "got": {}})
        p["got"][i] = rec["seq"]
        t = rec["t_us"]
        b["t0"] = t if b["t0"] is None else min(b["t0"], t)
        b["t1"] = t if b["t1"] is None else max(b["t1"], t)
    # Dentro de una ráfaga o de un hilo la secuencia debe crecer con i: el logger
    # numera y envía bajo el mismo mutex (el orden de llegada por UDP no importa).
    for b in bursts.values():
        for p in b["parts"].values():
            by_i = [p["got"][i] for i in sorted(p["got"])]
            p["order_ok"] = all(a < c for a, c in zip(by_i, by_i[1:]))
    totals = {"received": len(seqs), "lost": (max(seqs) + 1 - len(seqs)) if seqs else 0, "result": result}
    return bursts, console, totals


def report(bursts, console, totals, out=sys.stdout):
    rows = []
    ok = True
    for group in ["r1", "r2", "r3", "hilos"] + sorted(set(bursts) - {"r1", "r2", "r3", "hilos"}):
        b = bursts.get(group)
        if b is None:
            if group in MUST_BE_COMPLETE:
                ok = False
            rows.append((group, RATES.get(group, "4 hilos" if group == "hilos" else ""), "-", "0", "-", "-", "-", "-"))
            continue
        expected = sum(p["total"] for p in b["parts"].values())
        got = sum(len(p["got"]) for p in b["parts"].values())
        lost = expected - got
        order_ok = all(p["order_ok"] for p in b["parts"].values())
        span = (b["t1"] - b["t0"]) if got > 1 else 0
        rate = f"{(got - 1) * 1e6 / span:,.0f}/s" if span else "-"
        c = console.get(group)
        # Con ritmo fijo el tiempo por línea es la espera, no el costo
        cost = f"{c['us'] / max(1, expected):.2f} us" if c and group not in ("r2", "r3") else "-"
        errors = str(c["errors"]) if c else "-"
        label = RATES.get(group, f"{len(b['parts'])} hilos" if group == "hilos" else "")
        rows.append((group, label, str(expected), str(got), f"{lost} ({lost * 100 / expected:.2f} %)",
                     rate, cost, errors + ("" if order_ok else " ORDEN MAL")))
        if group in MUST_BE_COMPLETE and (lost or not order_ok):
            ok = False
    head = ("rafaga", "ritmo", "esperadas", "llegaron", "perdidas", "ritmo en consola", "costo/linea", "envios fallidos")
    widths = [max(len(h), *(len(r[i]) for r in rows)) for i, h in enumerate(head)]
    fmt = "  ".join(f"{{:<{w}}}" for w in widths)
    print(fmt.format(*head), file=out)
    print(fmt.format(*("-" * w for w in widths)), file=out)
    for r in rows:
        print(fmt.format(*r), file=out)
    print(f"\nlog completo: {totals['received']} lineas, {totals['lost']} perdidas en total, "
          f"{totals['result'] or 'sin RESULT'}", file=out)
    print("criterio: 0 perdidas en r2 (1000/s), r3 (100/s) y hilos -> " + ("CUMPLE" if ok else "NO CUMPLE"), file=out)
    return ok


def latest_log():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = sorted(glob.glob(os.path.join(here, "logs", "*_udplog_*.log")), key=os.path.getmtime)
    return files[-1] if files else None


def selftest():
    def check(cond, what):
        if not cond:
            raise SystemExit(f"udplog_stats selftest: FALLO: {what}")

    lines, seq = [], 0

    def add(level, msg, t):
        nonlocal seq
        lines.append(f"GCPS3 0000f00d {seq} {t} {level} udplog | {msg}")
        seq += 1

    add("I", "inicio", 0)
    for i in range(100):                      # r1: faltan 3 (sus seq no están)
        if i in (10, 11, 50):
            seq += 1
            continue
        add("D", f"r1 {i}/100", 1000 + i)
    add("I", "rafaga r1: 100 lineas en 99 us (0.99 us/linea, 1010101 lineas/s), 2 envios fallidos", 1100)
    for i in range(100):                      # r2: completa, 1000/s
        add("D", f"r2 {i}/100", 10000 + i * 1000)
    add("I", "rafaga r2: 100 lineas en 99000 us (990.00 us/linea, 1010 lineas/s), 0 envios fallidos", 110000)
    for i in range(100):                      # r3: completa
        add("D", f"r3 {i}/100", 200000 + i * 10000)
    for i in range(50):                       # hilos intercalados, completos y en orden
        for h in range(2):
            add("D", f"h{h} {i}/50", 2000000 + i)
    add("R", "RESULT: OK", 3000000)

    bursts, console, totals = analyze(lines)
    check(len(bursts["r1"]["parts"]["r1"]["got"]) == 97, "r1 con 3 perdidas")
    check(totals["lost"] == 3 and totals["result"] == "RESULT: OK", "totales")
    check(console["r1"]["errors"] == 2 and console["r2"]["us"] == 99000, "cifras de la consola")
    with open(os.devnull, "w") as null:
        check(report(bursts, console, totals, null), "cumple con r1 incompleta")
        lines.append("GCPS3 0000f00d 5001 3000001 D udplog | h2 0/2")   # h2: la línea 1 con
        lines.append("GCPS3 0000f00d 5000 3000002 D udplog | h2 1/2")   # seq menor que la 0
        b2, c2, t2 = analyze(lines)
        check(not report(b2, c2, t2, null), "orden roto en un hilo")
        b3, c3, t3 = analyze([ln for ln in lines if " r3 " not in ln])
        check(not report(b3, c3, t3, null), "sin r3 no cumple")
    print("udplog_stats selftest: OK")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()
    path = sys.argv[1] if len(sys.argv) > 1 else latest_log()
    if not path or not os.path.exists(path):
        print("No hay log de udplog: pasar la ruta, por ejemplo logs/20261018-120000_udplog_1234abcd.log")
        return 2
    with open(path, encoding="utf-8") as f:
        bursts, console, totals = analyze(f)
    if not bursts:
        print(f"{path}: no tiene lineas de rafagas")
        return 2
    print(f"{path}\n")
    return 0 if report(bursts, console, totals) else 1


if __name__ == "__main__":
    sys.exit(main())
