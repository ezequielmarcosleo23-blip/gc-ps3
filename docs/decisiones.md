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
