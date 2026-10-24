# Fase 2

**Objetivo:** que programas homebrew propios que dibujan con GX (triángulos, texturas, TEV, luces, copias del EFB) se vean en la app de PS3 igual que en el runner de PC, y que el runner dé la misma imagen que el renderizador por software de Dolphin. Sin software de Nintendo. Plan aprobado el 2026-10-23.

```
CPU emulada ─► FIFO de GX ─► Command Processor ─► vértices + XF ─► lista de dibujo
                              (BP, CP, XF)        (formatos,        (vértices en float,
                                                   matrices, luces)  estado, TEV)
                                                                          │
                                         ┌────────────────────────────────┴──────────┐
                            software (PC: runner, pruebas, CI)               RSX (PS3, rsxkit)
```

Todo lo anterior a la lista de dibujo es portable y se prueba en el CI; el RSX, en RPCS3.

## Etapas

| Etapa | Contenido | Se prueba con | Estado |
| --- | --- | --- | --- |
| A. FIFO y Command Processor | FIFO de GX en la RAM (write-gather pipe, punteros del PI y del CP, vuelta, marcas alta y baja), lector de comandos (registros de CP/XF/BP, cargas indexadas, display lists, invalidación de la caché de vértices, primitivas que por ahora solo se saltan), estado de BP/CP/XF, Pixel Engine (tokens, "draw done", interrupciones) | Unitarias, DOL `gx_fifo` con una mini-librería GX propia, traza de Dolphin | Completa (2026-10-24), ver abajo |
| B. Vértices y XF | Formatos de vértice (VAT/VCD, directo e índices de 8 y 16 bits), tipos de componente y de color, matrices indexadas, proyección, viewport, normales, coordenadas de textura, 8 luces | Unitarias con valores calculados; DOL `gx_tri` | Pendiente |
| C. Rasterizador de referencia (PC) | Primitivas, recorte, culling, Z en los formatos del EFB, scissor, TEV en enteros como el hardware, alpha test, mezcla, todos los formatos de textura con paleta y filtros, copias del EFB a la RAM (XFB y texturas) | DOLs `gx_tex`, `gx_tev`, `gx_light`, `gx_copy`; el XFB en la RAM contra el renderizador por software de Dolphin | Pendiente |
| D. Backend RSX | EFB como destino de rsxkit, vértices al RSX, TEV a fragment programs, texturas decodificadas (CMPR a DXT1), copias del EFB al XFB pasando por la RAM, en la app | RPCS3: la app anota la comparación de cada cuadro con el runner y `compare_log` la revisa | Pendiente |
| E. Cierre | Criterio de salida, documentos y decisiones | CI y RPCS3 | Pendiente |

## Etapa A: resultado (2026-10-24)

| Paso | Qué quedó |
| --- | --- |
| A1. FIFO | Gather pipe de 32 bytes a la RAM en el puntero del PI ([src/hw/gpfifo.c](../src/hw/gpfifo.c)); registros del CP con sus máscaras, marcas, breakpoint e interrupción ([src/hw/cp.c](../src/hw/cp.c)); la GPU en el hilo de la CPU con el evento SyncGPU, presupuesto y deuda ([src/gx/fifo.c](../src/gx/fifo.c)); PI_FIFO_RESET. El planificador corta el tramo de la CPU si llega un evento más cercano |
| A2. Lector y estado | Todos los comandos con sus costos, display lists, cargas indexadas, primitivas saltadas por su tamaño; estado de BP (con la máscara), CP y XF ([src/gx](../src/gx/README.md)) |
| A3. Pixel Engine | Token con y sin interrupción, "draw done", señales juntadas en un evento ([src/hw/pe.c](../src/hw/pe.c)) |
| A4. DOL | Mini-librería `tests/dol/gxlib.c` y DOL `gx_fifo` (10 secciones, [tests/dol](../tests/dol/README.md)). Igual a Dolphin en sus 124 441 instrucciones y con el mismo estado de GX al final (410 líneas: FIFO, BP, CP y XF). El parche de Dolphin y el runner vuelcan ese estado; las referencias de los 7 DOLs lo incluyen ([traza-dolphin.md](traza-dolphin.md)) |
| A5. Verificación | 19 pruebas unitarias nuevas y 2 de DOL (128 en total), OK en x86-64 con ASan/UBSan y en PowerPC 64; `make test-app` corre también `gx_fifo`. Mutaciones a mano detectadas: sin el corte del tramo, la marca alta después de sumar, el PE sin el mínimo de 500, 501 en vez de 500 y BP de 13 ciclos (estas dos, también en la traza de Dolphin). Decisión D-042 |

## Criterio de salida

1. Los DOLs de GX dan en el runner el mismo XFB que el renderizador por software de Dolphin (byte a byte donde se pueda; si no, con la tolerancia anotada).
2. Los mismos DOLs en RPCS3 (RSX) quedan dentro de una tolerancia acordada respecto del runner (el TEV calcula en enteros de 10 bits y el RSX en fp16/fp32: no pueden ser idénticos).
3. La traza de la CPU sigue igual a la de Dolphin en todos los DOLs (D-039).

## Decisiones (D-041)

| # | Decisión |
| --- | --- |
| 1 | Un rasterizador por software propio, solo para la PC: referencia, pruebas en el CI y depuración. No va en la PS3 |
| 2 | La verdad para las imágenes es el renderizador por software de Dolphin, comparando el XFB que queda en la RAM |
| 3 | DOLs de prueba con una mini-librería GX propia en `tests/dol`, sin libogc (como D-034 y D-035) |
| 4 | Intermedia ligera: el cargador de vértices, el XF y los decodificadores de textura se escriben listos para SPU (de un búfer a otro, en bloques de tamaño fijo, sin estado global ni punteros a la RAM emulada; quien los llama junta los datos), pero en esta fase corren solo en el PPU. Pasarlos a las SPU queda para la fase de rendimiento, con la consola |

## Riesgos

| Riesgo | Plan |
| --- | --- |
| El generador de rsxkit (7 términos, 2 texturas) no alcanza para el TEV (16 etapas, 8 texturas) | Generador propio en `src/gx`, o extender rsxkit (MIT) |
| Precisión del RSX frente a la del TEV | Tolerancia medida y documentada |
| RPCS3 puede no emular bien algo del RSX (formatos de textura, render a textura) | Probar cada cosa aparte; lo que falle se confirma en la consola |
| Con un solo hilo, Dolphin procesa el FIFO en momentos de su planificador: para que la traza siga igual hay que copiar ese momento | La traza de `gx_fifo` lo muestra, como pasó con el decrementador (D-040) |
| Es la fase más grande | Etapas cortas con pruebas, plan de cada etapa antes de empezarla |

## Fuera de la Fase 2

Juegos e ISOs, audio y DSP, el JIT, las SPU y la optimización.

## Reportes

Sin reportes técnicos por etapa.
