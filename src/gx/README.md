# src/gx

El lado de la GPU de la GameCube, portable: lee el FIFO de GX de la RAM, decodifica los comandos, guarda el estado de BP, CP y XF y carga y transforma los vértices de cada primitiva. Lo que la CPU ve de GX (gather pipe, registros del CP y del PE) está en [src/hw](../hw/README.md). Sigue a Dolphin (commit `cf5cb7ef`) con un solo hilo; lo portado lleva la atribución en el encabezado.

Fase 2 ([docs/fase2.md](../../docs/fase2.md)): etapas A (FIFO y estado) y B (vértices y XF). Las primitivas todavía no se dibujan: el viewport, el recorte y el rasterizador de referencia llegan en la etapa C, y el backend del RSX (`include/backend/gfx_backend.h`) en la D.

| Archivo | Qué hace | En Dolphin |
| --- | --- | --- |
| `gx.h` | El estado de la GPU (`gc_gx_t`) y su API | |
| `fifo.c` | La GPU en el hilo de la CPU: evento SyncGPU, presupuesto y deuda, lectura de bloques de 32 bytes al búfer de video, sincronización al leer registros | `VideoCommon/Fifo.cpp` |
| `opcode.c` | El lector de comandos y lo que cuesta cada uno | `VideoCommon/OpcodeDecoding.h/.cpp` |
| `bp.c` | Registros de BP: máscara (0xFE), token, "draw done" | `VideoCommon/BPStructs.cpp` |
| `cpmem.c` | Registros de CP por el FIFO (VCD, VAT, arreglos, matrices) y el tamaño de un vértice | `CPMemory.cpp`, `VertexLoaderBase.cpp`, `VertexLoader_*.h` |
| `xf.c` | Cargas de XF, directas e indexadas | `VideoCommon/XFStructs.cpp` |
| `prim.c` | Una primitiva: partes de 16 380, vértices descartados, los que usa, la caché de normales, la foto del XF, bloques de 64; el volcado en texto | `VertexLoaderManager.cpp`, `VertexManagerBase.cpp`, `IndexGenerator.cpp`, `SWVertexLoader.cpp` |
| `vtx.h`, `vtx.c` | Cargador de vértices: juntar (PPU) y decodificar (listo para SPU) | `VertexLoaderX64.cpp`, `SWVertexLoader.cpp` |
| `xform.h`, `xform.c` | El XF: posición, proyección, normales, color y alfa con 8 luces, coordenadas de textura (listo para SPU) | `VideoBackends/Software/TransformUnit.cpp` |
| `estado.c` | El estado en texto para compararlo con Dolphin | El parche de `tools/dolphin-trace` |

## Tiempo (D-042)

| Momento | Qué pasa |
| --- | --- |
| La CPU completa un bloque en el gather pipe | Se copia a la RAM en el acto; con el FIFO enlazado suma 32 a la distancia y despierta la GPU |
| La GPU se despierta | Evento SyncGPU 1000 ciclos después, con 1000 de presupuesto |
| SyncGPU | Lee bloques mientras haya datos, no esté en el breakpoint y el presupuesto no sea negativo. Si se pasa, la deuda queda y vuelve deuda + 1000 ciclos después; si no, se duerme |
| La CPU lee SR, la mitad alta de la distancia o del puntero de lectura | La GPU corre 1000 ciclos en el acto |
| Token o "draw done" | El PE cambia max(500, ciclos acumulados en el bloque) después |

| Comando | Bytes | Ciclos |
| --- | --- | --- |
| NOP (0x00), seguidos | n | 6 por byte |
| CP (0x08) | 6 | 12 |
| XF (0x10) | 5 + 4 por palabra | 18 + 6 por palabra |
| XF indexada (0x20, 0x28, 0x30, 0x38) | 5 | 6 |
| Display list (0x40) | 9 | 6 + su contenido |
| BP (0x61) | 5 | 12 |
| 0x44, 0x48 (INVL_VC) | 1 | 6 |
| Primitiva (0x80-0xBF) | 3 + vértices | 12 por vértice + 6 (cargarla no cambia el tiempo) |
| Desconocido | 1 | 1 |

Un comando que no entra entero en lo leído espera al bloque siguiente en el búfer de video (2 MB, como Dolphin). Una display list dentro de otra se ignora; lo que no entra entero al final de una lista se pierde.

## Estado

| Qué | Cómo se guarda |
| --- | --- |
| BP | 256 palabras de 24 bits; el valor nuevo pasa por la máscara (0xFE), que vuelve a 0xFFFFFF después de cualquier otra escritura. Los registros que disparan algo (copia del EFB, cargas de TLUT y de TMEM) se guardan y se avisan una vez en el log |
| CP | VCD, VAT (8 formatos × 3 grupos), 16 arreglos (base de 26 bits, paso de 8) y los índices de matrices. Se elige por los 4 bits de arriba del comando: 0x31 también es MATINDEX_A |
| XF | 0x1058 palabras: matrices, luces y registros. Una carga que pasa del último registro se corta; una indexada lee de la RAM con la base y el paso del arreglo 12 a 15 |

## Vértices (D-043)

La referencia es lo que transforma el renderizador por software de Dolphin, bit a bit. Cada primitiva pasa por:

| Paso | Dónde corre | Qué hace |
| --- | --- | --- |
| Plan | PPU (`gx_vtx_setup`) | Del VCD y el VAT de la primitiva: el tamaño en el FIFO, los pasos para juntar y cómo decodificar. Se rehace cuando cambian VCD o ese VAT |
| Juntar | PPU (`gx_vtx_gather`) | Copia cada atributo directo y reemplaza cada índice por los bytes de su arreglo en la RAM; queda un búfer de vértices de tamaño fijo. Una posición con índice 0xFF o 0xFFFF descarta el vértice |
| Decodificar | Listo para SPU (`gx_vtx_decode`) | Bloques de hasta 64 vértices a float: escalas, colores a RGBA de 8 bits, lo que falta desde la caché de normales y los índices de matriz de XF |
| Transformar | Listo para SPU (`gx_xform`) | El mismo bloque con una foto del XF (unos 7 KB: matrices, luces, registros y las escalas de BP), las mismas operaciones de float en el mismo orden que Dolphin |

Listo para SPU quiere decir búfer a búfer, sin estado global ni punteros a la RAM emulada; por ahora corre en el PPU, con la FPU de IEEE que pide la comparación con Dolphin (la de la SPU redondea hacia cero). Todo se compila con `-ffp-contract=off`: con FMA (fmadds del PPU) los vértices ya no son los de Dolphin.

| Detalle | Cómo es |
| --- | --- |
| Escala de posición y coordenadas enteras | 2^-fracción solo con ByteDequant (bit 30 del grupo A); sin él, el entero tal cual (el cargador x64 de Dolphin) |
| Escala de normales enteras | Siempre 2^-7, 2^-6, 2^-15 o 2^-14 (u8, s8, u16, s16) |
| Índices de matriz | Del vértice: posición con 6 bits, textura con los 8; si no, de XF 0x1018 y 0x1019 (no de CP) |
| Colores | 565, 888, 888x, 4444, 6666 y 8888 a RGBA; sin color, blanco; solo el color 1 va al canal 0 |
| Normales que faltan | Las de la caché: normal y tangente del último vértice de la parte si no se descartó, binormal del último cargado |
| Vértices que se transforman | Los que usa la primitiva, siempre los primeros: 3 × n/3 en triángulos, 4 × n/4 en cuadrados más 3 si sobran 3, 2 × n/2 en líneas, todos en tiras, abanicos y puntos (si alcanzan para una) |
| XF y BP distintos | Si no coinciden la cantidad de coordenadas (XF 0x103F y BP genMode) o de canales (XF 0x1009), no se transforma nada, como Dolphin |
| Momento | Al leer la primitiva, con el estado de ese momento. Dolphin transforma al vaciar el lote; da lo mismo porque cada cambio del XF lo vacía antes, salvo escribir XF 0x1018/0x1019 con el valor que ya tenía el registro de CP |

`--vertices ARCHIVO` en el runner escribe cada vértice transformado en el formato del parche de Dolphin (`GCPS3_VERTICES`): bits de la posición en la vista y proyectada, las 3 normales, los 2 colores y las coordenadas, con NaN siempre como `7fc00000`.

## Pruebas

`tests/unit/test_gx.c` (19 pruebas), `test_vtx.c` (10), `test_xform.c` (12) y `test_prim.c` (6), y los DOLs `gx_fifo` y `gx_tri` ([tests/dol](../../tests/dol/README.md)), iguales a Dolphin instrucción por instrucción, con el mismo estado de GX al final y, en `gx_tri`, los mismos 374 vértices bit a bit ([docs/traza-dolphin.md](../../docs/traza-dolphin.md)).
