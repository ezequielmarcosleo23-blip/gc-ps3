#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# gc-ps3 - emulador de GameCube para PlayStation 3
# Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
"""Compara lo que hizo la app de PS3 (en RPCS3 o en la consola) con el runner.

Lee un log de la app: el TTY.log de RPCS3 o el archivo de tools/logrecv
(logs/*.log). Por cada programa que corrió la app ("app: corriendo X (reloj R)"
... "app: fin de X, C ciclos, ...") corre el runner de PC con el mismo DOL, el
mismo reloj y los mismos ciclos, y compara:

  - la salida del programa, línea por línea;
  - la lista de cuadros distintos ("cuadro N (campo F) WxH, crc32 RGB 0x...").

También dice si la salida es la de tests/dol/esperado/X.txt (si el programa
corrió lo suficiente). Sale con 0 si todo coincide y 1 si no.

    python3 tools/compare_log.py logs/20261020-101500_gc-ps3_1a2b3c4d.log
    python3 tools/compare_log.py --selftest
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RUNNER = os.path.join(ROOT, "build", "host", "gc-ps3-runner")
DOLS = os.path.join(ROOT, "tests", "dol", "bin")
EXPECTED = os.path.join(ROOT, "tests", "dol", "esperado")

UDP = re.compile(r"^GCPS3 [0-9a-f]{8} \d+ \d+ ([IWERD]) \S+ \| (.*)$")
START = re.compile(r"^app: corriendo (\S+) \(reloj (\d+)\)$")
END = re.compile(r"^app: fin de (\S+), (\d+) ciclos,")
FRAME = re.compile(r"^(?:app|runner): (cuadro \d+ \(campo \d+\) \d+x\d+, crc32 RGB 0x[0-9a-f]{8})$")


def messages(lines):
    """El texto de cada línea: el mensaje de una línea UDP o la línea de la TTY."""
    for raw in lines:
        line = raw.rstrip("\r\n")
        m = UDP.match(line)
        if m:
            if m.group(1) == "D":
                continue
            line = m.group(2)
        yield line


def runs(lines):
    """Cada corrida de la app: nombre, reloj, ciclos, salida y cuadros."""
    cur = None
    for line in messages(lines):
        m = START.match(line)
        if m:
            cur = {"name": m.group(1), "rtc": int(m.group(2)), "output": [], "frames": []}
            continue
        if cur is None:
            continue
        m = END.match(line)
        if m:
            if m.group(1) == cur["name"]:
                cur["cycles"] = int(m.group(2))
                yield cur
            cur = None
            continue
        m = FRAME.match(line)
        if m:
            cur["frames"].append(m.group(1))
        elif not line.startswith(("app: ", "emu: ", "[W] ", "[E] ")):
            cur["output"].append(line)


def run_runner(runner, dol, rtc, cycles):
    p = subprocess.run([runner, "--ciclos", str(cycles), "--rtc", str(rtc), "--cuadros", dol],
                       capture_output=True, text=True, errors="replace")
    frames = [m.group(1) for m in (FRAME.match(l) for l in p.stderr.splitlines()) if m]
    return p.stdout.splitlines(), frames


def first_difference(a, b):
    for k in range(max(len(a), len(b))):
        x = a[k] if k < len(a) else "(nada)"
        y = b[k] if k < len(b) else "(nada)"
        if x != y:
            return k + 1, x, y
    return None


def compare(log_lines, runner=RUNNER, dols=DOLS, expected=EXPECTED, out=sys.stdout):
    ok_all, count = True, 0
    for r in runs(log_lines):
        count += 1
        dol = os.path.join(dols, r["name"])
        print(f"== {r['name']}: {r['cycles']} ciclos, reloj {r['rtc']}", file=out)
        if not os.path.exists(dol):
            print(f"   no encuentro {dol}", file=out)
            ok_all = False
            continue
        want_out, want_frames = run_runner(runner, dol, r["rtc"], r["cycles"])
        for what, got, want in (("salida", r["output"], want_out), ("cuadros", r["frames"], want_frames)):
            diff = first_difference(got, want)
            if diff is None:
                print(f"   {what}: {len(got)} líneas iguales al runner", file=out)
            else:
                k, x, y = diff
                print(f"   {what}: DISTINTA en la línea {k}\n     PS3:    {x}\n     runner: {y}", file=out)
                ok_all = False
        exp = os.path.join(expected, os.path.splitext(r["name"])[0] + ".txt")
        if os.path.exists(exp):
            with open(exp, encoding="utf-8") as f:
                exp_lines = f.read().splitlines()
            if r["output"] == exp_lines:
                print("   salida esperada (tests/dol/esperado): completa", file=out)
            elif r["output"] == exp_lines[:len(r["output"])]:
                print(f"   salida esperada: {len(r['output'])} de {len(exp_lines)} líneas "
                      "(cortó antes de terminar)", file=out)
            else:
                print("   salida esperada: DISTINTA", file=out)
                ok_all = False
    if count == 0:
        print("no hay corridas de la app en el log", file=out)
        return False
    print("RESULTADO: " + ("todo igual al runner" if ok_all else "HAY DIFERENCIAS"), file=out)
    return ok_all


def selftest(runner=RUNNER):
    """Arma un log como el de la app con el runner y comprueba que el
    comparador lo acepta y que ve una salida o un cuadro cambiados."""
    import io
    if not os.path.exists(runner):
        print(f"compare_log selftest: falta el runner {runner} (make host)")
        return 1
    log = ["app: gc-ps3 (simulado)"]
    for name, cycles in (("hola.dol", 5000000), ("pantalla.dol", 30000000)):
        out, frames = run_runner(runner, os.path.join(DOLS, name), 1234, cycles)
        log.append(f"app: corriendo {name} (reloj 1234)")
        log.append("emu: hle: 4 funciones enganchadas desde el .map")
        log += out
        log += ["app: " + f for f in frames]
        log.append(f"app: fin de {name}, {cycles} ciclos, 9 campos, {len(frames)} cuadros distintos")
    udp = [f"GCPS3 0000beef {k} {k * 10} I gc-ps3 | {m}" for k, m in enumerate(log)]
    sink = io.StringIO()
    checks = [
        ("TTY", compare(log, runner=runner, out=sink)),
        ("UDP", compare(udp, runner=runner, out=sink)),
        ("salida cambiada", not compare([l.replace("hola desde", "chau desde") for l in log], runner=runner, out=sink)),
        ("cuadro cambiado", not compare([re.sub(r"0x10589e8c", "0x10589e8d", l) for l in log], runner=runner, out=sink)),
        ("sin corridas", not compare(["nada"], runner=runner, out=sink)),
    ]
    bad = [n for n, ok in checks if not ok]
    if bad:
        print(sink.getvalue())
        print("compare_log selftest: FALLA en " + ", ".join(bad))
        return 1
    print("compare_log selftest: OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", nargs="?", help="TTY.log de RPCS3 o log de logrecv")
    ap.add_argument("--runner", default=RUNNER, help="el runner de PC (build/host/gc-ps3-runner)")
    ap.add_argument("--dols", default=DOLS, help="carpeta de los .dol (y .map)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.runner)
    if not a.log:
        ap.error("falta el log")
    with open(a.log, encoding="utf-8", errors="replace") as f:
        return 0 if compare(f.readlines(), runner=a.runner, dols=a.dols) else 1


if __name__ == "__main__":
    sys.exit(main())
