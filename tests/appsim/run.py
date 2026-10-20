#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# gc-ps3 - emulador de GameCube para PlayStation 3
# Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
"""Corre la app de PS3 simulada en la PC (make test-app) y revisa su log.

La app (build/host/appsim/gc-ps3-appsim, ver tests/appsim/stubs.c) recorre el
menú con un guion de botones: contador, cpu, hw, pantalla y timer, con dos
vueltas por el menú del sistema y un cierre desde el sistema con timer
corriendo. Con el reloj fijo el log sale siempre igual. Se revisa:

  - tools/compare_log.py: cada corrida igual al runner (salida y cuadros);
  - el orden de las corridas y el exit(0) de cpu, hw y timer;
  - el cuadro de pantalla (CRC 0x10589e8c) y varios cuadros de contador;
  - que cada cuadro avisado se subió a rsxkit y que la subida (XRGB8888,
    vuelta a RGB) tiene el mismo CRC;
  - que la app volvió del menú del sistema y salió por EXIT_GAME.

    python3 tests/appsim/run.py build/host/appsim/gc-ps3-appsim [--runner RUNNER]
"""
import os
import argparse
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import compare_log  # noqa: E402

UP, DOWN, CROSS, CIRCLE = 0x10, 0x40, 0x4000, 0x2000
# Menú, por nombre: contador, cpu, hola, hw, pantalla, timer.
# Cada cuadro de la PS3 corre 3 millones de ciclos (15 tramos de 200 000). Un botón
# en dos cuadros seguidos es uno solo apretado: entre dos toques va un cuadro libre.
SCRIPT = [
    (1, UP),                      # ya está arriba: no se mueve
    (2, CROSS),                   # contador, 17 cuadros (unos 6 campos)
    (20, CIRCLE),
    (22, DOWN), (24, CROSS),      # cpu
    (28, CIRCLE),
    (30, DOWN), (32, DOWN), (34, CROSS),   # hw
    (38, "xmb"),                  # menú del sistema con hw ya terminado
    (40, CIRCLE),
    (42, DOWN), (43, DOWN),       # pantalla: mantenido dos cuadros, baja uno
    (44, CROSS),                  # pantalla, 13 cuadros
    (52, "xmb"),                  # menú del sistema con pantalla corriendo
    (58, CIRCLE),
    (60, DOWN), (62, DOWN),       # timer; el segundo DOWN ya está abajo
    (64, CROSS),
    (68, "salir"),                # el sistema cierra la app con timer corriendo
]
RELOJ = 1792475992                # 845791192 segundos desde 2000
RUNS = ["contador.dol", "cpu.dol", "hw.dol", "pantalla.dol", "timer.dol"]
PANTALLA_CRC = "0x10589e8c"

FRAME_CRC = re.compile(r"^app: cuadro \d+ \(campo \d+\) (\d+x\d+), crc32 RGB (0x[0-9a-f]{8})$")
UPLOAD_CRC = re.compile(r"^sim: subida \d+ (\d+x\d+), crc32 RGB (0x[0-9a-f]{8})$")
SIM_STATS = re.compile(r"^sim: (\d+) cuadros de la PS3, (\d+) subidas, (\d+) dibujos, (\d+) vueltas del XMB$")


def guion():
    return ",".join(f"{f}:{b}" if isinstance(b, str) else f"{f}:{b:x}" for f, b in SCRIPT)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("app", help="build/host/appsim/gc-ps3-appsim")
    ap.add_argument("--runner", default=compare_log.RUNNER)
    a = ap.parse_args()
    app = os.path.abspath(a.app)
    work = os.path.dirname(app)
    dols = os.path.join(work, "dol")
    shutil.rmtree(dols, ignore_errors=True)
    os.makedirs(dols)
    for name in sorted(os.listdir(compare_log.DOLS)):
        if name.endswith((".dol", ".map")):
            shutil.copy(os.path.join(compare_log.DOLS, name), dols)

    env = dict(os.environ, GUION=guion(), RELOJ=str(RELOJ))
    env.pop("TEXTO", None)
    p = subprocess.run(["./" + os.path.basename(app)], cwd=work, env=env, capture_output=True,
                       text=True, errors="replace", timeout=600)
    log, sim = p.stdout.splitlines(), p.stderr.splitlines()
    with open(os.path.join(work, "app.log"), "w", encoding="utf-8") as f:
        f.write(p.stdout)
    with open(os.path.join(work, "sim.txt"), "w", encoding="utf-8") as f:
        f.write(p.stderr)

    bad = []

    def check(ok, what):
        if not ok:
            bad.append(what)

    check(p.returncode == 0, f"la app salió con {p.returncode}")
    check(compare_log.compare(log, runner=a.runner), "compare_log ve diferencias con el runner")
    runs = list(compare_log.runs(log))
    by_name = {r["name"]: r for r in runs}
    check([r["name"] for r in runs] == RUNS, "corridas " + " ".join(r["name"] for r in runs))
    for name in ("cpu.dol", "hw.dol", "timer.dol"):
        check(f"app: {name} termino con exit(0)" in log, f"{name} sin exit(0)")
    pantalla = by_name.get("pantalla.dol", {}).get("frames", [])
    check(len(pantalla) == 1 and pantalla[0].endswith(PANTALLA_CRC), f"cuadros de pantalla {pantalla}")
    contador = by_name.get("contador.dol", {}).get("frames", [])
    check(len(contador) >= 5, f"solo {len(contador)} cuadros de contador")

    frames = [m.groups() for m in map(FRAME_CRC.match, log) if m]
    uploads = [m.groups() for m in map(UPLOAD_CRC.match, sim) if m]
    check(frames and uploads == frames,
          f"{len(uploads)} subidas a rsxkit para {len(frames)} cuadros, o con otro CRC")
    stats = [m for m in map(SIM_STATS.match, sim) if m]
    check(len(stats) == 1 and int(stats[0].group(3)) > 0, "sin dibujos del cuadro")
    check(len(stats) == 1 and int(stats[0].group(4)) == 2, "no volvió dos veces del menú del sistema")
    last = SCRIPT[-1][0]
    check(len(stats) == 1 and int(stats[0].group(1)) == last + 1, f"no salió en el cuadro {last} (EXIT_GAME)")
    check(log[-1:] == ["app: salida"], "no terminó con \"app: salida\"")

    if bad:
        print("\n".join(sim[-5:]))
        print(f"test-app: FALLA ({work}/app.log)\n  - " + "\n  - ".join(bad))
        return 1
    print(f"test-app: OK ({len(runs)} corridas, {len(frames)} cuadros, log en {work}/app.log)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
