# src/core

Núcleo portable: compila igual en la PC y en la PS3.

| Archivo | Qué hace |
| --- | --- |
| `version.h` | Nombre y versión |
| `endian.h` | Acceso big-endian (copia directa en la PS3, inversión en la PC) |
| `log.h/.c` | Log del núcleo; la plataforma elige el destino (stderr o UDP) |
| `logline.h/.c` | Formato de línea del log UDP |
| `pattern.h/.c` | Patrón de posición de la prueba `iso` |
| `mem.h/.c` | RAM, ARAM, caché bloqueada y mapa de direcciones ([mapa-memoria](../../docs/mapa-memoria.md)) |
| `mmio.h/.c` | Registros de hardware: manejadores por bloque |
| `dol.h/.c` | Lector de ejecutables DOL: validación y copia a la RAM |
| `sched.h/.c` | Planificador de eventos en ciclos de CPU |
| `hle.h/.c` | Ganchos al inicio de funciones del programa (opcode 1) y lector de `.map` con el formato de Dolphin |
| `hle_os.c` | Los ganchos que se reconocen por nombre: `OSReport`, `printf`, `vprintf`, `puts`, `__write_console`, `exit` |
| `vaformat.h/.c` | printf con el formato en la RAM emulada y los argumentos según el ABI SVR4 del PowerPC |
| `system.h/.c` | La máquina armada (memoria, CPU, planificador y el hardware de [src/hw](../hw/README.md)), arranque HLE de un DOL ([arranque-hle](../../docs/arranque-hle.md)) y el bucle que corre la CPU entre eventos |
| `xfb.h/.c` | El cuadro que muestra la VI: los dos campos línea por medio y YUV 4:2:2 a RGB (BT.601, enteros) |
| `png.h/.c` | PNG sin compresión ni librerías, para guardar cuadros |
| `crc32.h/.c` | CRC-32 de zlib (PNG y comparación de cuadros) |
| `frames.h/.c` | Al terminar cada campo de video arma el cuadro RGB y avisa si cambió (número, campo, tamaño y CRC); lo usan el runner (`--cuadros`) y la app de PS3 |

Cada módulo tiene sus pruebas en `tests/unit/` (`make test` y `make test-be`).
