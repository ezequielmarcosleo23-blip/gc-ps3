# Registro de decisiones

Una entrada por decisión, con la fecha, lo elegido y por qué. Si una decisión cambia, se agrega una entrada nueva que la reemplaza; no se borra la anterior.

| # | Fecha | Decisión | Motivo |
| --- | --- | --- | --- |
| D-001 | 2026-10-18 | Licencia GPL-2.0-or-later | Permite portar código de Dolphin y del núcleo de Salsa64 e integrar librerías MIT |
| D-002 | 2026-10-18 | C11 en núcleo, SPU y plataforma | Mismo lenguaje en PPU, SPU y PC; sin dependencias de C++ en el toolchain |
| D-003 | 2026-10-18 | PSL1GHT + toolchain de ps3dev, nightly fijado (`tools/toolchain.env`) con SHA-256 | Build reproducible sin SDK de Sony |
| D-004 | 2026-10-18 | Núcleo portable + `src/platform/ps3` + runner en PC | La lógica se prueba en PC con ASan/UBSan; la consola queda para JIT, SPU y rendimiento |
| D-005 | 2026-10-18 | Arranque por HLE del apploader; IPL opcional y nunca distribuido | Sin dependencia de archivos con copyright |
| D-006 | 2026-10-18 | Consolas de prueba: una con HEN y otra con CFW en modo DEX | Cubren el caso de los usuarios y el de desarrollo |
| D-007 | 2026-10-18 | PPE para la CPU de Gekko; SPE para vértices, texturas y DSP; RSX para dibujar | La FPU de Gekko necesita IEEE (la SPU redondea hacia cero) |
| D-008 | 2026-10-18 | Memoria emulada con direccionamiento enmascarado, sin fallos de página | lv2 bloquea la notificación de fallos (EINVAL) |
| D-009 | 2026-10-18 | Entorno de PC: Ubuntu 24.04 en WSL2 con el toolchain en `/usr/local/ps3dev` | Lo que recomienda ps3dev en Windows; verificado 15/15 y 28/28 |
| D-010 | 2026-10-18 | Para depurar: `gdb-multiarch` + `ppu-/spu-addr2line` en lugar del GDB 8.3.1 del nightly | El GDB del nightly se cae al iniciar con Python 3.12 |
| D-011 | 2026-10-18 | CI: GitHub Actions construye `docker/Dockerfile` con caché y compila dentro | Mismo entorno que `make docker-make` en la PC, sin publicar imágenes |
| D-012 | 2026-10-18 | Repo en `C:\GCNS3 EMU\gc-ps3`, público desde el inicio | Claude edita ahí directamente; Actions sin límite de minutos en repos públicos |
| D-013 | 2026-10-18 | Logger UDP propio y mínimo | La licencia de dbglogger está sin revisar |
| D-014 | 2026-10-18 | Prueba `iso` de la Fase 0 con un archivo sintético de 1.4 GB | Se verifica cada byte sin copia de referencia y sin problemas legales; la ISO real llega en la Fase 1 |
| D-015 | 2026-10-18 | Fase 0 solo en la consola HEN; las pruebas que requieren CFW/DEX quedan pendientes | Por ahora solo hay una consola con HEN |
| D-016 | 2026-10-18 | RPCS3 como consola provisional para validar lo funcional | La consola HEN quedó fuera de servicio; JIT y rendimiento esperan al hardware |
| D-017 | 2026-10-18 | El log se envía a broadcast y a 127.0.0.1 (o a la IP de `log.cfg`) y el receptor descarta duplicados | Funciona sin configurar nada en RPCS3 y en la red local |
| D-018 | 2026-10-18 | Dos receptores con el mismo formato: `logrecv.py` (WSL/CI) y `logrecv.ps1` (Windows) | En Windows no hace falta instalar nada ni cambiar la red de WSL para usar RPCS3 |
| D-019 | 2026-10-18 | spukit y rsxkit v0.1.0 (MIT) en `lib/`, desde sus repos propios, compiladas con su Makefile original | Se evalúan en las pruebas `spu` y `rsx`; la decisión de adoptarlas se toma al cerrar la Fase 0 |
| D-020 | 2026-10-18 | Provisional: `libcg` (NVIDIA Cg 3.1, Ubuntu multiverse) solo al compilar, para los 11 shaders precompilados de rsxkit | `cgcomp` la carga con dlopen. Cambia lo descartado antes ("Cg: solo x86 y descontinuado"): las máquinas de build son todas x86-64 y no se distribuye. Alternativa si molesta: versionar los `.vpo`/`.fpo` ya compilados |
| D-021 | 2026-10-18 | Logger seguro entre hilos (`lwmutex`) y nivel `D` de detalle (solo UDP, sin pantalla) | Las pruebas `spu`, `rsx` y el emulador loguean desde varios hilos; las ráfagas de datos no deben frenar la TTY ni llenar la consola del receptor |
| D-022 | 2026-10-18 | Caché del JIT en una cola NOBITS de 32 MB al final del segmento de texto (`-T nobits.ld -T lv2.ld`) | Único lugar escribible y ejecutable con HEN según Salsa64; 32 MB es el alcance de un salto relativo. Se valida en consola con la prueba `jit` |
