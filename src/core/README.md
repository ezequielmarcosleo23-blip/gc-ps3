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

Cada módulo tiene sus pruebas en `tests/unit/` (`make test` y `make test-be`).
