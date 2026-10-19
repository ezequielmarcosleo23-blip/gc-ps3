# Fase 0

**Objetivo:** demostrar en consola real que funcionan las piezas críticas (JIT, SPU, RSX y lectura de discos), con un repo que compila igual en la PC y en GitHub Actions. Todavía no se emula nada de GameCube.

## Etapas

| Etapa | Contenido | Estado |
| --- | --- | --- |
| A. Repo y build | Esqueleto GPLv2+, Makefile PPU/SPU/PC, ps3mapi-lib fijada, imagen Docker, GitHub Actions | Completa (2026-10-18): CI verde, prueba de humo OK en RPCS3 |
| B. Base de pruebas | Detección de entorno, log por UDP con `RESULT: OK/FAIL`, receptores en la PC, `make run-test` | Validada en RPCS3 (2026-10-18); falta la parte de consola: ps3load, buzzer, HEN |
| C. Las 5 pruebas | `udplog`, `jit`, `spu`, `rsx`, `iso` | Validada en RPCS3 (2026-10-18): `udplog`, `spu`, `rsx` e `iso` OK; `jit` y las cifras de rendimiento esperan a la consola |
| D. Cierre | Resultados, `perf-log.md`, decisiones y plan de la Fase 1 | En curso (2026-10-19): resultados, decisiones y lista para la consola listos; falta D-020 (Cg) y el plan de la Fase 1 |

## Pruebas en consola

| # | Prueba | Qué hace | Éxito | HEN | DEX |
| --- | --- | --- | --- | --- | --- |
| 1 | `udplog` | 3 ráfagas de 10 000 líneas (sin pausa, 1000/s, 100/s) y 4 hilos a la vez; `tools/udplog_stats.py` cuenta las perdidas de cada una | 0 perdidas a 1000/s, 100/s y con hilos; costo por línea medido | — | — |
| 2 | `jit` | Escribe, ejecuta, modifica y vuelve a ejecutar una función PPC en una cola NOBITS de 32 MB; marca de crash en disco. Después: syscall 905, `page_allocate` y `sys/dbg.h` | Cada método da OK/FAIL sin colgar la consola | — | pendiente (sin consola DEX) |
| 3 | `spu` | 64 000 vértices × matriz 4×4 con DMA de doble búfer: hilo SPU directo, spukit con 1 y con 4 trabajadores | Bit a bit igual al PPU con valores exactos; vértices/s; con valores cualquiera se informan las ULP de diferencia | — | — |
| 4 | `rsx` | Destino de 640×480 (el EFB) con dos triángulos, uno con fragment program generado en runtime, presentado a 4:3 sobre 720p con texto | Píxeles correctos leídos del búfer de color, captura de pantalla; tiempo por cuadro | — | — |
| 5 | `iso` | Archivo de 1 459 978 240 bytes (`tools/mkiso.py`, cada palabra = su posición) desde HDD y USB: secuencial de 1 MB y 1000 lecturas de 32 KB al azar, verificando cada byte | Íntegro y muy por encima de ~3 MB/s | — | — |

## Sin consola por ahora

La consola HEN quedó fuera de servicio el 2026-10-18. Mientras tanto RPCS3 hace de consola provisional para todo lo funcional; el JIT y las mediciones de rendimiento esperan al hardware real.

| Parte | En RPCS3 | Espera a la consola |
| --- | --- | --- |
| Log UDP, base común, receptores | Sí | — |
| `make run-test` con ps3load, buzzer, detección de HEN | — | Sí |
| `udplog`, `spu`, `rsx`, `iso` | Solo que funcionen | Rendimiento |
| `jit` | — | Sí |

### Puerta de la Fase 0 en consola

Lo que falta para dar la Fase 0 por cerrada en hardware real. Las cifras van a [perf-log.md](perf-log.md).

- [ ] `jit` en HEN: la cola NOBITS se lee, se ejecuta y se modifica sin colgar la consola
- [ ] `jit` en CFW: el mismo método y después syscall 905 y `page_allocate` de PS3MAPI
- [ ] `make run-test` con ps3loadx y el receptor en WSL (modo *mirrored* o `logrecv.cmd`)
- [ ] Buzzer (1 pitido OK, 3 FAIL) y detección de HEN, Cobra y Mamba
- [ ] `iso` desde USB (`/dev_usb000/gcps3/patron.bin`)
- [ ] Rendimiento de `udplog`, `spu`, `rsx`, `iso` y `jit` en `perf-log.md`
- [ ] Confirmar con esas cifras la adopción de spukit y rsxkit (D-024)

## Cómo correr cada prueba en RPCS3

Siempre con `tools\logrecv.cmd` abierto y File → Boot (S)ELF → `build\phase0\<prueba>.self` (ver [log-udp.md](log-udp.md)).

| Prueba | Antes de arrancarla | Dura | Después |
| --- | --- | --- | --- |
| `udplog` | — | ~2 min | `python3 tools/udplog_stats.py` en Ubuntu (lee el log más reciente) |
| `spu` | — | segundos | — |
| `iso` | Generar el archivo en `dev_hdd0/gcps3/patron.bin` de RPCS3 (no en `tmp`, que RPCS3 vacía): `python3 tools/mkiso.py <ruta>` | según el disco | — |
| `rsx` | Configuration → GPU → **Write Color Buffers** activado (si no, la lectura de píxeles da FAIL) | 10 s | Captura de pantalla |
| `jit` | Solo en consola | — | — |

## Resultados

| Fecha | Prueba | Dónde | Resultado |
| --- | --- | --- | --- |
| 2026-10-18 | Prueba de humo de la app (`gc-ps3.self`) | RPCS3 v0.0.42 | `spu: hello OK (1.5 6.0 0.0 44.0)`, `RESULT: OK` |
| 2026-10-18 | `hello`: log UDP hasta la PC (`logrecv.cmd`) | RPCS3 v0.0.42 | 13 líneas, 0 perdidas, 0 envíos fallidos; `RESULT: OK` |
| 2026-10-18 | `udplog` | RPCS3 v0.0.42 + `logrecv.cmd` | 40 017 líneas, 0 perdidas en las 4 ráfagas (sin pausa, 1000/s, 100/s, 4 hilos) y 0 envíos fallidos; sin pausa 67 µs/línea (14 885 líneas/s); con 4 hilos, secuencia en orden; `RESULT: OK` |
| 2026-10-18 | `spu` | RPCS3 v0.0.42 | Con valores exactos, hilo directo, spukit x1 y x4 dan los mismos bits que el PPU. Con valores cualquiera difieren 161 848 de 256 000 componentes: lo esperado si la SPU redondea hacia cero (una simulación en la PC da 161 491); `RESULT: OK` |
| 2026-10-18 | `rsx` | RPCS3 v0.0.42 (Write Color Buffers) | Salida 1280×720; destino 640×480 presentado en x=160, 960×720 (4:3); píxeles leídos correctos (`ff3020`, degradado, `202020`, franja `000000`); 1 fragment program generado en runtime; 600 cuadros, 17,4 ms de media; captura con los dos triángulos y el texto; `RESULT: OK` |
| 2026-10-18 | `iso` | RPCS3 v0.0.42 | FAIL: no encontró `/dev_hdd0/tmp/gcps3/patron.bin`. RPCS3 vacía `dev_hdd0/tmp` al arrancar cada programa: los datos pasan a `/dev_hdd0/gcps3` (D-023) y se repite |
| 2026-10-18 | `spu` (repetida, nueva métrica) | RPCS3 v0.0.42 | Igual que antes con valores exactos. Con valores cualquiera, error máximo 4,58 u (u = 2^-24 de la escala de cada cuenta), el mismo que da la simulación de redondeo hacia cero en la PC: RPCS3 emula el redondeo de la SPU y la diferencia con el PPU es de unos pocos redondeos; `RESULT: OK` |
| 2026-10-18 | `iso` (repetida) | RPCS3 v0.0.42, archivo en `E:` | 1 459 978 240 bytes en `/dev_hdd0/gcps3/patron.bin`: secuencial de 1 MB y 1000 lecturas de 32 KB al azar, todos los bytes coinciden; USB omitido (sin archivo); `RESULT: OK`. Velocidades (65 MB/s secuencial, 9,8 ms por lectura al azar) son las del disco de la PC |

Se llenan conforme se ejecutan las pruebas. Los tiempos medidos en RPCS3 dependen de la PC y no cuentan como rendimiento; las cifras de la consola van en [perf-log.md](perf-log.md).
