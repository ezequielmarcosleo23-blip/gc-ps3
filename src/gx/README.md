# src/gx

El lado de la GPU de la GameCube, portable: lee el FIFO de GX de la RAM, decodifica los comandos, guarda el estado de BP, CP y XF y carga y transforma los vértices de cada primitiva. Lo que la CPU ve de GX (gather pipe, registros del CP y del PE) está en [src/hw](../hw/README.md). Sigue a Dolphin (commit `cf5cb7ef`) con un solo hilo; lo portado lleva la atribución en el encabezado.

Fase 2 ([docs/fase2.md](../../docs/fase2.md)): etapas A (FIFO y estado), B (vértices y XF) y, en curso, C (el renderizador por software de Dolphin: el armado, el recorte, el rasterizador, el TEV, la mezcla con el EFB y la caja envolvente; faltan las texturas y la conversión de las copias a la RAM). El backend del RSX (`include/backend/gfx_backend.h`) llega en la D.

| Archivo | Qué hace | En Dolphin |
| --- | --- | --- |
| `gx.h` | El estado de la GPU (`gc_gx_t`) y su API | |
| `fifo.c` | La GPU en el hilo de la CPU: evento SyncGPU, presupuesto y deuda, lectura de bloques de 32 bytes al búfer de video, sincronización al leer registros | `VideoCommon/Fifo.cpp` |
| `opcode.c` | El lector de comandos y lo que cuesta cada uno | `VideoCommon/OpcodeDecoding.h/.cpp` |
| `bp.c` | Registros de BP: máscara (0xFE), token, "draw done", tijera, caja envolvente, colores del TEV | `VideoCommon/BPStructs.cpp`, `PixelShaderManager.cpp` |
| `cpmem.c` | Registros de CP por el FIFO (VCD, VAT, arreglos, matrices) y el tamaño de un vértice | `CPMemory.cpp`, `VertexLoaderBase.cpp`, `VertexLoader_*.h` |
| `xf.c` | Cargas de XF, directas e indexadas; el viewport cambia la tijera | `VideoCommon/XFStructs.cpp` |
| `prim.c` | Una primitiva: partes de 16 380, vértices descartados, los que usa, la caché de normales, la foto del XF, bloques de 64; el volcado en texto | `VertexLoaderManager.cpp`, `VertexManagerBase.cpp`, `IndexGenerator.cpp`, `SWVertexLoader.cpp` |
| `vtx.h`, `vtx.c` | Cargador de vértices: juntar (PPU) y decodificar (listo para SPU) | `VertexLoaderX64.cpp`, `SWVertexLoader.cpp` |
| `xform.h`, `xform.c` | El XF: posición, proyección, normales, color y alfa con 8 luces, coordenadas de textura (listo para SPU) | `VideoBackends/Software/TransformUnit.cpp` |
| `raster.h`, `setup.c` | Armado de triángulos, líneas y puntos; recorte, culling, división por w y viewport | `IndexGenerator.cpp`, `VideoBackends/Software/SetupUnit.cpp`, `Clipper.cpp` |
| `raster.c` | El rasterizador: rectángulos de tijera, aristas en 28.4, bloques de 2 × 2, pendientes, LOD, zfreeze, Z temprana; el volcado de triángulos | `VideoBackends/Software/Rasterizer.cpp`, `BPFunctions.cpp` |
| `tev.h`, `tev.c` | El TEV: etapas indirectas, combinadores, constantes, swap, prueba de alfa, textura de Z, niebla, Z tardía (las texturas dan negro hasta C4) | `VideoBackends/Software/Tev.cpp` |
| `efb.h`, `efb.c` | El EFB: 640 × 528 de color y Z en los formatos de píxel; prueba de Z, mezcla, resta, lógicas, alfa de destino, dither | `VideoBackends/Software/SWEfbInterface.cpp` |
| `bbox.c` | La caja envolvente con la caché de VideoCommon | `VideoCommon/BoundingBox.cpp`, `SWBoundingBox.cpp` |
| `copy.c` | BP 0x52: rectángulo, tamaño en la RAM, borrado; el volcado de copias; la lectura del EFB por el CPU | `BPStructs.cpp`, `TextureCacheBase.cpp`, `BPFunctions.cpp`, `EfbCopy.cpp`, `EFBInterface.cpp` |
| `x86.h` | Las conversiones de float a entero como en x86-64 (fuera de rango o NaN) | |
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

`tests/unit/test_gx.c` (19 pruebas), `test_vtx.c` (10), `test_xform.c` (12), `test_prim.c` (6), `test_efb.c` (5), `test_setup.c` (5) y `test_raster.c` (7), y los DOLs `gx_fifo`, `gx_tri` y `gx_raster` ([tests/dol](../../tests/dol/README.md)), iguales a Dolphin instrucción por instrucción, con el mismo estado de GX y EFB al final y los mismos vértices, triángulos y copias bit a bit ([docs/traza-dolphin.md](../../docs/traza-dolphin.md)).

## Del triángulo al píxel (etapa C3)

| Paso | Cómo es |
| --- | --- |
| Tijera | BP 0x20, 0x21 y 0x59 con el viewport dan hasta 3 × 3 rectángulos (desplazamientos de -4096 a 4096 de a 1024), ordenados por el área que cubren del viewport (el mejor, último). Se recalculan al dibujar si cambió algo y después de una copia con borrado |
| Aristas | Cada vértice a 28.4 con `iround(16 × x) - 9`; convención de relleno de Dolphin; bloques de 2 × 2 dentro del rectángulo |
| Valores | Pendientes en float desde el primer vértice con el ajuste de 0,495: Z (con recorte a 24 bits), 1/w, los colores y las coordenadas (en perspectiva con q) en s17.7 de 24 bits |
| Z | Temprana en el rasterizador (BP 0x43 bit 6) o tardía después de la niebla; sin prueba no se escribe aunque la máscara lo deje |
| TEV | En enteros como Dolphin; su estado queda de un píxel a otro (ver docs/fase2.md) |
| Mezcla | Factores con el bit de arriba sumado (0 a 256), resta, las 16 lógicas, alfa de destino, dither de Bayer solo en RGBA6, máscaras de color y alfa |
| CPU | 0xC8000000: color en ARGB con el modo de alfa del PE (BP 0x43 da el formato) o Z de 24 bits; lo de 8 y 16 bits se queda con los bits de abajo y lo de 64 los pone en la parte baja; escribir no hace nada |
| Caja envolvente | La caché de VideoCommon (con `BBoxEnable`) y las coordenadas que actualiza cada píxel, redondeadas a 2 × 2 |

Las conversiones de float a entero siguen a x86-64 (`x86.h`) y la aritmética entera que en Dolphin puede desbordar se hace con la vuelta de 32 bits. Lo que en Dolphin escribe fuera de sus arreglos (más de 2 canales, 8 coordenadas o 4 etapas indirectas en genMode) se recorta a esos máximos.
