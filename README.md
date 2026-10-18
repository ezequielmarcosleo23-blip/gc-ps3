# gc-ps3

Emulador de GameCube para PlayStation 3, escrito en C11 sobre [PSL1GHT](https://github.com/ps3dev/PSL1GHT) y el toolchain abierto de [ps3dev](https://github.com/ps3dev/ps3dev). Sin SDK ni herramientas de Sony.

> **Estado: Fase 0.** Todavía no emula nada: esta fase prueba en consola real las piezas críticas (JIT, SPU, RSX, E/S y log por red). Ver [docs/fase0.md](docs/fase0.md).

## Compilar

Requisitos: Linux (o Ubuntu en WSL) con el toolchain `nightly-2026-10-12` de ps3dev en `/usr/local/ps3dev` y las variables `PS3DEV` y `PSL1GHT` definidas.

```bash
make                 # app de PS3 (build/ps3/gc-ps3.self) y runner de PC
make test            # pruebas unitarias en PC con ASan + UBSan
make pkg             # paquete instalable (build/ps3/gc-ps3.pkg)
make run PS3LOAD=tcp:192.168.1.50   # envía el .self a ps3loadx en la consola
make toolchain-check # comprueba que el toolchain es el fijado en tools/toolchain.env
make docker-make     # compila todo dentro de la imagen Docker, igual que el CI
```

## Estructura

```
src/core/            memoria, MMIO, scheduler, DVD, arranque HLE (portable)
src/cpu/             intérprete de Gekko (el JIT después)
src/gx/              Command Processor, BP/XF/TEV (portable)
src/dsp/             HLE de AX
src/spu/             programas SPE (cada carpeta es un programa)
src/platform/ps3/    código específico de la PS3
src/platform/host/   runner en PC
include/backend/     interfaces propias (gfx_backend.h, spu_jobs.h)
lib/                 librerías de terceros fijadas a un commit (ver lib/VERSIONS.md)
tests/unit/          pruebas unitarias del núcleo (PC)
tests/phase0/        pruebas de la Fase 0 en consola
docker/              imagen de build (la misma en la PC y en CI)
tools/               toolchain fijado y scripts
docs/                decisiones, fases, presupuesto de memoria y perf-log
```

## Licencia

GPL-2.0-or-later (ver [COPYING](COPYING)). Cada archivo fuente lleva el encabezado GPL con la cláusula "or (at your option) any later version". Las librerías en `lib/` conservan su propia licencia, compatible con la GPL.

Este proyecto no incluye ni distribuye BIOS/IPL, juegos ni ningún software de Nintendo o Sony.
