# Fase 0

**Objetivo:** demostrar en consola real que funcionan las piezas críticas (JIT, SPU, RSX y lectura de discos), con un repo que compila igual en la PC y en GitHub Actions. Todavía no se emula nada de GameCube.

## Etapas

| Etapa | Contenido | Estado |
| --- | --- | --- |
| A. Repo y build | Esqueleto GPLv2+, Makefile PPU/SPU/PC, ps3mapi-lib fijada, imagen Docker, GitHub Actions | Completa (2026-10-18): CI verde, prueba de humo OK en RPCS3 |
| B. Base de pruebas | Detección de entorno, log por UDP con `RESULT: OK/FAIL`, receptores en la PC, `make run-test` | En curso: validación en RPCS3 mientras no haya consola |
| C. Las 5 pruebas | `udplog`, `jit`, `spu`, `rsx`, `iso` | Pendiente |
| D. Cierre | Resultados, `perf-log.md`, decisiones y plan de la Fase 1 | Pendiente |

## Pruebas en consola

| # | Prueba | Qué hace | Éxito | HEN | DEX |
| --- | --- | --- | --- | --- | --- |
| 1 | `udplog` | 10 000 mensajes numerados a distintas velocidades | 0 perdidos a ritmo normal; latencia medida | — | — |
| 2 | `jit` | Escribe, ejecuta, modifica y vuelve a ejecutar una función PPC. Métodos: cola NOBITS, syscall 905, `page_allocate`. Marca de crash en disco; prueba `sys/dbg.h` | Cada método da OK/FAIL sin colgar la consola | — | pendiente (sin consola DEX) |
| 3 | `spu` | Vértices por DMA → transformación 4×4 → DMA de regreso; hilos SPU directos y spukit | Bit a bit igual al PPU; vértices/s | — | — |
| 4 | `rsx` | Triángulo con rsxkit (programas generados en runtime, sin Cg), 720p con RESC | Triángulo en pantalla; tiempo por frame | — | — |
| 5 | `iso` | Archivo sintético de 1.4 GB desde HDD y USB: secuencial, aleatorio de 32 KB y CRC | Íntegro y muy por encima de ~3 MB/s | — | — |

## Sin consola por ahora

La consola HEN quedó fuera de servicio el 2026-10-18. Mientras tanto RPCS3 hace de consola provisional para todo lo funcional; el JIT y las mediciones de rendimiento esperan al hardware real.

| Parte | En RPCS3 | Espera a la consola |
| --- | --- | --- |
| Log UDP, base común, receptores | Sí | — |
| `make run-test` con ps3load, buzzer, detección de HEN | — | Sí |
| `udplog`, `spu`, `rsx`, `iso` | Solo que funcionen | Rendimiento |
| `jit` | — | Sí |

## Resultados

| Fecha | Prueba | Dónde | Resultado |
| --- | --- | --- | --- |
| 2026-10-18 | Prueba de humo de la app (`gc-ps3.self`) | RPCS3 v0.0.42 | `spu: hello OK (1.5 6.0 0.0 44.0)`, `RESULT: OK` |

Se llenan conforme se ejecutan las pruebas. Las cifras de rendimiento van también en [perf-log.md](perf-log.md).
