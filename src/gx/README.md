# src/gx

El lado de la GPU de la GameCube, portable: lee el FIFO de GX de la RAM, decodifica los comandos y guarda el estado de BP, CP y XF. Lo que la CPU ve de GX (gather pipe, registros del CP y del PE) está en [src/hw](../hw/README.md). Sigue a Dolphin (commit `cf5cb7ef`) con un solo hilo; lo portado lleva la atribución en el encabezado.

Fase 2, etapa A ([docs/fase2.md](../../docs/fase2.md)): las primitivas todavía no se dibujan, se saltan con el tamaño de vértice que dan VCD y VAT. Los vértices y el XF llegan en la etapa B, el rasterizador de referencia en la C y el backend del RSX (`include/backend/gfx_backend.h`) en la D.

| Archivo | Qué hace | En Dolphin |
| --- | --- | --- |
| `gx.h` | El estado de la GPU (`gc_gx_t`) y su API | |
| `fifo.c` | La GPU en el hilo de la CPU: evento SyncGPU, presupuesto y deuda, lectura de bloques de 32 bytes al búfer de video, sincronización al leer registros | `VideoCommon/Fifo.cpp` |
| `opcode.c` | El lector de comandos y lo que cuesta cada uno | `VideoCommon/OpcodeDecoding.h/.cpp` |
| `bp.c` | Registros de BP: máscara (0xFE), token, "draw done" | `VideoCommon/BPStructs.cpp` |
| `cpmem.c` | Registros de CP por el FIFO (VCD, VAT, arreglos, matrices) y el tamaño de un vértice | `CPMemory.cpp`, `VertexLoaderBase.cpp`, `VertexLoader_*.h` |
| `xf.c` | Cargas de XF, directas e indexadas | `VideoCommon/XFStructs.cpp` |
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
| Primitiva (0x80-0xBF) | 3 + vértices | 12 por vértice + 6 |
| Desconocido | 1 | 1 |

Un comando que no entra entero en lo leído espera al bloque siguiente en el búfer de video (2 MB, como Dolphin). Una display list dentro de otra se ignora; lo que no entra entero al final de una lista se pierde.

## Estado

| Qué | Cómo se guarda |
| --- | --- |
| BP | 256 palabras de 24 bits; el valor nuevo pasa por la máscara (0xFE), que vuelve a 0xFFFFFF después de cualquier otra escritura. Los registros que disparan algo (copia del EFB, cargas de TLUT y de TMEM) se guardan y se avisan una vez en el log |
| CP | VCD, VAT (8 formatos × 3 grupos), 16 arreglos (base de 26 bits, paso de 8) y los índices de matrices. Se elige por los 4 bits de arriba del comando: 0x31 también es MATINDEX_A |
| XF | 0x1058 palabras: matrices, luces y registros. Una carga que pasa del último registro se corta; una indexada lee de la RAM con la base y el paso del arreglo 12 a 15 |

## Pruebas

`tests/unit/test_gx.c` (19 pruebas) y el DOL `gx_fifo` ([tests/dol](../../tests/dol/README.md)), igual a Dolphin instrucción por instrucción y con el mismo estado de GX al final ([docs/traza-dolphin.md](../../docs/traza-dolphin.md)).
