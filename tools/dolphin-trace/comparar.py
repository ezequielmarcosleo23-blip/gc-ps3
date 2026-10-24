#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# gc-ps3 - emulador de GameCube para PlayStation 3
# Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
"""Corre DOLs en el runner y en el Dolphin parchado y compara sus huellas.

Por cada DOL: el runner corre con --huella (hasta exit() o --instrucciones),
Dolphin corre la misma cantidad de instrucciones con el mismo reloj y la misma
configuración del hardware (docs/traza-dolphin.md), y tools/trace_diff.py
compara. Si difieren, repite las dos con el detalle de esas instrucciones y
dice qué registros cambian. Después compara el estado de GX que los dos dejan
al final (FIFO, BP, CP y XF; la CPU no los ve) y cada vértice que transforman
(posición, normales, colores y coordenadas, bit a bit). Con --referencia
guarda los puntos, el estado y los vértices de Dolphin en esa carpeta
(tests/dol/dolphin), que es lo que revisa make test.

    python3 tools/dolphin-trace/comparar.py --dolphin build/dolphin/build/Binaries/dolphin-emu-nogui \\
        tests/dol/bin/hola.dol tests/dol/bin/cpu.dol
    python3 tools/dolphin-trace/comparar.py ... --instrucciones 30000000 tests/dol/bin/contador.dol
"""
import argparse
import io
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import trace_diff  # noqa: E402

RUNNER = os.path.join(ROOT, "build", "host", "gc-ps3-runner")
DOLPHIN = os.path.join(ROOT, "build", "dolphin", "build", "Binaries", "dolphin-emu-nogui")
GC_EPOCH_UNIX = 946684800
RELOJ = 845791192                     # segundos desde 2000, el mismo de las pruebas
MAX_INSTRUCCIONES = 200_000_000       # los que terminan con exit() paran antes

# La configuración de Dolphin que corresponde al arranque de gc-ps3 (D-032, D-036)
DOLPHIN_CONFIG = [
    "Dolphin.Core.CPUCore=0",              # intérprete
    "Dolphin.Core.CPUThread=False",
    "Dolphin.Core.SkipIPL=True",
    "Dolphin.Core.DSPHLE=True",
    "Dolphin.Core.SlotA=255",              # sin tarjetas de memoria
    "Dolphin.Core.SlotB=255",
    "Dolphin.Core.SerialPort1=255",
    "Dolphin.Core.SIDevice0=6",            # mando estándar en el puerto 1
    "Dolphin.Core.SIDevice1=0",
    "Dolphin.Core.SIDevice2=0",
    "Dolphin.Core.SIDevice3=0",
    "Dolphin.Core.FallbackRegion=1",       # NTSC-U: sin esto, el país del sistema (PAL si no hay)
    "Dolphin.Core.SelectedLanguage=0",     # inglés, el de la SRAM de gc-ps3
    "Dolphin.Core.EnableCustomRTC=True",
    "Dolphin.Core.EmulationSpeed=0",
    "Dolphin.Core.GFXBackend=Software Renderer",   # la referencia de las imágenes (D-041, D-043)
    "Dolphin.DSP.Backend=No Audio Output",
    "Dolphin.Interface.ConfirmStop=False",
]


def last_point(puntos):
    """(instrucciones, pc, cadena) de la última línea de un archivo de puntos."""
    with open(puntos, encoding="utf-8") as f:
        lines = [l.split() for l in f if l.strip() and not l.startswith("#")]
    if not lines:
        return 0, 0, 0
    n, pc, cadena = lines[-1]
    return int(n), int(pc, 16), int(cadena, 16)


def drop_detail(base):
    """Un .detalle viejo confundiría a trace_diff: se borra antes de cada corrida."""
    if os.path.exists(base + ".huella.detalle"):
        os.remove(base + ".huella.detalle")


def run_runner(runner, dol, rtc, limit, base, detalle=None):
    drop_detail(base)
    cmd = [runner, "--rtc", str(rtc), "--ciclos", str(limit), "--huella", base + ".huella",
           "--puntos", base + ".puntos", "--estado-gx", base + ".gx", "--vertices", base + ".vtx",
           dol]
    if detalle:
        cmd[1:1] = ["--detalle", detalle]
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if p.returncode not in (0,) and "termino con exit" not in p.stderr:
        raise RuntimeError(f"el runner falló ({p.returncode}):\n{p.stderr}")
    return last_point(base + ".puntos")[0]


def run_dolphin(dolphin, dol, rtc, count, base, work, detalle=None):
    drop_detail(base)
    if os.path.exists(base + ".vtx"):      # Dolphin lo crea recién al dibujar
        os.remove(base + ".vtx")
    user = os.path.join(work, "dolphin-user")
    shutil.rmtree(user, ignore_errors=True)
    os.makedirs(os.path.join(user, "Maps"))
    # Sin dispositivos de entrada (headless) el mando 1 figuraría desconectado:
    # "Always Connected" lo deja enchufado, sin botones y con las palancas al centro
    os.makedirs(os.path.join(user, "Config"))
    with open(os.path.join(user, "Config", "GCPadNew.ini"), "w", encoding="utf-8") as f:
        f.write("[GCPad1]\nOptions/Always Connected = True\n")
    stem = os.path.splitext(os.path.basename(dol))[0]
    mapfile = os.path.splitext(dol)[0] + ".map"
    if os.path.exists(mapfile):
        shutil.copy(mapfile, os.path.join(user, "Maps", stem + ".map"))
    env = dict(os.environ, GCPS3_HUELLA=base + ".huella", GCPS3_PUNTOS=base + ".puntos",
               GCPS3_LIMITE=str(count), GCPS3_ESTADO_GX=base + ".gx",
               GCPS3_VERTICES=base + ".vtx")
    env.pop("GCPS3_DETALLE", None)
    if detalle:
        env["GCPS3_DETALLE"] = detalle
    cmd = [dolphin, "-p", "headless", "-u", user, "-e", os.path.abspath(dol)]
    for c in DOLPHIN_CONFIG + [f"Dolphin.Core.CustomRTCValue={rtc + GC_EPOCH_UNIX}"]:
        cmd += ["-C", c]
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace", timeout=3600)
    with open(base + ".log", "w", encoding="utf-8") as f:
        f.write(p.stdout + p.stderr)
    if "gc-ps3: huella de" not in p.stderr:
        raise RuntimeError(f"Dolphin no terminó la traza ({p.returncode}); ver {base}.log")
    return last_point(base + ".puntos")[0]


def read_estado(path):
    """Las líneas del estado de GX, sin comentarios."""
    with open(path, encoding="utf-8") as f:
        return [l.rstrip("\n") for l in f if l.strip() and not l.startswith("#")]


def compare_estado(ra, da):
    """Compara el estado de GX de las dos corridas; True si es el mismo."""
    a, b = read_estado(ra + ".gx"), read_estado(da + ".gx")
    if a == b:
        print(f"   estado de GX igual ({len(a)} líneas)")
        return True
    only_a = [l for l in a if l not in b]
    only_b = [l for l in b if l not in a]
    print("   estado de GX DISTINTO:")
    for l in only_a[:20]:
        print(f"     runner : {l}")
    for l in only_b[:20]:
        print(f"     dolphin: {l}")
    return False


def read_vertices(path):
    """Las líneas de vértices (vacío si Dolphin no dibujó nada)."""
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [l.rstrip("\n") for l in f if l.strip() and not l.startswith("#")]


def vertex_fields(line):
    """Los campos de una línea de vértice con su nombre."""
    t = line.split()
    names = ["v.x", "v.y", "v.z", "p.x", "p.y", "p.z", "p.w"]
    names += [f"n{k}.{c}" for k in range(3) for c in "xyz"]
    names += ["color0", "color1"]
    vals = t[1:4] + t[5:9] + t[10:19] + t[20:22]
    out = dict(zip(names, vals))
    if len(t) > 22:
        out["ntex"] = t[22]
        for k, i in enumerate(range(23, len(t), 3)):
            for c, v in zip("stq", t[i:i + 3]):
                out[f"t{k}.{c}"] = v
    return out


def compare_vertices(ra, da):
    """Compara los vértices de las dos corridas; True si son los mismos."""
    a, b = read_vertices(ra + ".vtx"), read_vertices(da + ".vtx")
    if a == b:
        print(f"   vértices iguales ({len(a)})")
        return True
    i = next((k for k, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    print(f"   vértices DISTINTOS: {len(a)} en el runner, {len(b)} en Dolphin; desde el {i}")
    if i < len(a) and i < len(b):
        fa, fb = vertex_fields(a[i]), vertex_fields(b[i])
        for k in fb:
            if fa.get(k) != fb[k]:
                print(f"     {k}: runner {fa.get(k)}, dolphin {fb[k]}")
    for name, lines in (("runner", a), ("dolphin", b)):
        if i < len(lines):
            print(f"     {name}: {lines[i]}")
    return False


def compare(dol, o):
    stem = os.path.splitext(os.path.basename(dol))[0]
    ra, da = os.path.join(o.trabajo, stem + ".runner"), os.path.join(o.trabajo, stem + ".dolphin")
    n = run_runner(o.runner, dol, o.rtc, o.instrucciones, ra)
    m = run_dolphin(o.dolphin, dol, o.rtc, n, da, o.trabajo)
    print(f"== {stem}: {n} instrucciones en el runner, {m} en Dolphin")
    first = io.StringIO()
    idx = trace_diff.report(ra + ".huella", da + ".huella", ("runner", "dolphin"), out=first)
    if idx is None or idx >= min(n, m):
        print(first.getvalue(), end="")
    if idx is not None and idx < min(n, m):
        window = f"{max(0, idx - 4)}:{idx + 2}"
        run_runner(o.runner, dol, o.rtc, n, ra, window)
        run_dolphin(o.dolphin, dol, o.rtc, n, da, o.trabajo, window)
        trace_diff.report(ra + ".huella", da + ".huella", ("runner", "dolphin"))
        return False
    if idx is not None:
        return False
    same_estado = compare_estado(ra, da)
    if not compare_vertices(ra, da) or not same_estado:
        return False
    if o.referencia:
        os.makedirs(o.referencia, exist_ok=True)
        out = os.path.join(o.referencia, stem + ".txt")
        with open(da + ".puntos", encoding="utf-8") as f:
            points = [l for l in f if not l.startswith("#")]
        with open(out, "w", encoding="utf-8") as f:
            f.write(f"# gc-ps3 huella v1: Dolphin cf5cb7ef paso a paso (tools/dolphin-trace)\n")
            f.write(f"# dol {stem}.dol\n# reloj {o.rtc}\n# instrucciones {m}\n")
            f.writelines(points)
        gx = os.path.join(o.referencia, stem + ".gx.txt")
        with open(gx, "w", encoding="utf-8") as f:
            f.write(f"# gc-ps3 estado gx v1: Dolphin cf5cb7ef al final de la traza (tools/dolphin-trace)\n")
            f.write(f"# dol {stem}.dol\n# reloj {o.rtc}\n# instrucciones {m}\n")
            f.writelines(l + "\n" for l in read_estado(da + ".gx"))
        vtx = os.path.join(o.referencia, stem + ".vtx.txt")
        vertices = read_vertices(da + ".vtx")
        if vertices:
            with open(vtx, "w", encoding="utf-8") as f:
                f.write(f"# gc-ps3 vertices v1: Dolphin cf5cb7ef, video por software (tools/dolphin-trace)\n")
                f.write(f"# dol {stem}.dol\n# reloj {o.rtc}\n# instrucciones {m}\n")
                f.writelines(l + "\n" for l in vertices)
        elif os.path.exists(vtx):
            os.remove(vtx)
        print(f"   referencia en {os.path.relpath(out, ROOT)}, {os.path.relpath(gx, ROOT)}"
              + (f" y {os.path.relpath(vtx, ROOT)}" if vertices else ""))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dols", nargs="+")
    ap.add_argument("--dolphin", default=DOLPHIN)
    ap.add_argument("--runner", default=RUNNER)
    ap.add_argument("--rtc", type=int, default=RELOJ, help="segundos desde el 2000-01-01")
    ap.add_argument("--instrucciones", type=int, default=MAX_INSTRUCCIONES)
    ap.add_argument("--trabajo", default=os.path.join(ROOT, "build", "dolphin", "trazas"))
    ap.add_argument("--referencia", help="carpeta donde guardar los puntos (tests/dol/dolphin)")
    o = ap.parse_args()
    os.makedirs(o.trabajo, exist_ok=True)
    ok = all([compare(d, o) for d in o.dols])
    print("RESULTADO: " + ("todas iguales a Dolphin" if ok else "HAY DIFERENCIAS"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
