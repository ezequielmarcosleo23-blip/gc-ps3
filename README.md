# gc-ps3

Emulador de GameCube para PlayStation 3, escrito en C11 sobre [PSL1GHT](https://github.com/ps3dev/PSL1GHT) y el toolchain abierto de [ps3dev](https://github.com/ps3dev/ps3dev). Sin SDK ni herramientas de Sony.

> **Estado: Fase 2.** La Fase 0 (JIT, SPU, RSX, E/S y log por red) está validada en RPCS3 y espera la consola para el JIT y el rendimiento ([docs/fase0.md](docs/fase0.md)). La Fase 1 (memoria, intérprete de Gekko, hardware mínimo e imagen en la PS3) cumplió su criterio de salida: la app corre DOLs propios en RPCS3 igual que el runner, y el intérprete da el mismo estado que Dolphin instrucción por instrucción ([docs/fase1.md](docs/fase1.md), [docs/app-ps3.md](docs/app-ps3.md), [docs/traza-dolphin.md](docs/traza-dolphin.md)). La Fase 2 lleva GX al RSX ([docs/fase2.md](docs/fase2.md)): las etapas A (FIFO de GX, Command Processor y Pixel Engine) y B (vértices y XF: formatos, matrices, luces y coordenadas de textura) están completas e iguales a Dolphin, la B vértice por vértice; la C (el rasterizador de referencia: triángulos, TEV, texturas, EFB y copias al XFB y a textura) también, bit a bit con el renderizador por software de Dolphin, y por ahora es lo que dibuja en la PS3. Sigue la D: dibujar con el RSX.

## Compilar

Requisitos: Linux (o Ubuntu en WSL) con el toolchain `nightly-2026-10-12` de ps3dev en `/usr/local/ps3dev`, las variables `PS3DEV` y `PSL1GHT` definidas y `libcg` instalado (`sudo apt install libcg`; lo usa `cgcomp` para los shaders de rsxkit).

```bash
make                 # app de PS3 (build/ps3/gc-ps3.self) y runner de PC
make test            # pruebas unitarias en PC con ASan + UBSan (incluye los DOLs de tests/dol)
build/host/gc-ps3-runner tests/dol/bin/cpu.dol   # corre un DOL en el intérprete (--help)
build/host/gc-ps3-runner --ciclos 45000000 --imagen pantalla.png tests/dol/bin/pantalla.dol   # y guarda la imagen
build/host/gc-ps3-runner --ciclos 90000000 --cuadros tests/dol/bin/contador.dol   # lista los cuadros con su CRC
make test-app        # la app de PS3 simulada en la PC con un guion de botones (también en make test)
python3 tools/compare_log.py logs/<log de la app>.log   # compara lo que hizo la app en RPCS3 con el runner
make dolphin && make dolphin-ref   # Dolphin con el parche de traza y las referencias de tests/dol/dolphin (Linux, ~1 h)
make phase0          # pruebas de la Fase 0 para consola o RPCS3 (build/phase0/*.self)
make logrecv         # recibe el log UDP de la consola (en Windows: tools\logrecv.cmd)
make pkg             # paquete instalable con los DOLs de prueba (build/ps3/gc-ps3.pkg; ver docs/app-ps3.md)
make run PS3LOAD=tcp:192.168.1.50   # envía el .self a ps3loadx en la consola
make run-test TEST=hello PS3LOAD=tcp:192.168.1.50   # envía una prueba y espera su RESULT
make toolchain-check # comprueba el toolchain fijado en tools/toolchain.env y cgcomp
python3 tools/mkiso.py patron.bin   # archivo de 1.4 GB para la prueba iso
make docker-make     # compila todo dentro de la imagen Docker, igual que el CI
```

## Estructura

```
src/core/            memoria, MMIO, scheduler, DVD, arranque HLE (portable)
src/cpu/             intérprete de Gekko (el JIT después)
src/hw/              hardware alrededor de la CPU: PI, VI, SI, EXI, DSP, AI, DI, MI, gather pipe, CP y PE (portable)
src/gx/              la GPU: FIFO de GX, comandos, estado de BP, CP y XF, carga y transformación de vértices (portable)
src/dsp/             HLE de AX
src/spu/             programas SPE (cada carpeta es un programa)
src/platform/ps3/    código específico de la PS3
src/platform/host/   runner en PC
include/backend/     interfaces propias (gfx_backend.h, spu_jobs.h)
lib/                 librerías de terceros fijadas a un commit (ver lib/VERSIONS.md)
tests/unit/          pruebas unitarias del núcleo (PC)
tests/appsim/        la app de PS3 corriendo en la PC con versiones de prueba de lv2, mando y rsxkit
tests/dol/           DOLs de prueba propios (sin libc), su salida esperada y la huella de Dolphin
tests/phase0/        pruebas de la Fase 0 en consola
docker/              imagen de build (la misma en la PC y en CI)
tools/               toolchain fijado y scripts
docs/                decisiones, fases, presupuesto de memoria y perf-log
```

## Licencia

GPL-2.0-or-later (ver [COPYING](COPYING)). Cada archivo fuente lleva el encabezado GPL con la cláusula "or (at your option) any later version". Las librerías en `lib/` conservan su propia licencia, compatible con la GPL.

Este proyecto no incluye ni distribuye BIOS/IPL, juegos ni ningún software de Nintendo o Sony.
