# Comparación con Dolphin, instrucción por instrucción

Los DOLs de prueba corren en el intérprete de gc-ps3 y en Dolphin (cf5cb7ef, con un parche chico), y el estado de la CPU tiene que ser el mismo antes de cada instrucción. Es el segundo punto del criterio de salida de la Fase 1 (D-039).

## Cómo funciona

| Pieza | Qué hace |
| --- | --- |
| `tools/dolphin-trace/traza.patch` | Parche del intérprete de Dolphin. Con `GCPS3_HUELLA` o `GCPS3_PUNTOS` en el entorno, avanza instrucción por instrucción (como el paso a paso del depurador) y anota la huella de cada una; sin esas variables corre como siempre. Corrige además dos errores del intérprete (ver abajo) |
| `tools/dolphin-trace/build.sh` | Baja Dolphin, aplica el parche y compila `dolphin-emu-nogui` sin interfaz, video ni audio (`make dolphin`) |
| `src/cpu/huella.c` | La misma huella en gc-ps3 |
| `build/host/gc-ps3-runner --huella/--puntos/--detalle` | La huella del runner |
| `tools/trace_diff.py` | Compara dos huellas y muestra la primera instrucción distinta y qué registros cambian |
| `tools/dolphin-trace/comparar.py` | Corre cada DOL en los dos con el mismo reloj, compara y, si son iguales, guarda la referencia (`make dolphin-ref`) |
| `tests/dol/dolphin/*.txt` | Las referencias: la huella encadenada de Dolphin cada 65 536 instrucciones y al final |
| `tests/dol/dolphin/*.gx.txt` | El estado de GX de Dolphin al final de la traza (ver abajo) |
| `tests/unit/test_dolphin.c` | `make test` y el CI corren los DOLs y comparan con las referencias, sin Dolphin |

## Por qué paso a paso

Dolphin tiene dos maneras de llevar el tiempo:

| | Bucle normal del intérprete | Paso a paso (depurador) | gc-ps3 |
| --- | --- | --- | --- |
| Ciclos por instrucción | Según la tabla (`mulli` 3, `divw` 40...) | 1 | 1 |
| Eventos e interrupciones | Al terminar cada bloque | Entre cada instrucción | Entre cada instrucción |

La traza sale del paso a paso: el mismo modelo de tiempo que gc-ps3, así que una diferencia es un error de semántica (CPU o hardware) y no una diferencia de reloj. Adoptar el tiempo por bloques de Dolphin se decide con el JIT.

## Formato (versión 1)

El estado son 89 palabras de 64 bits, tomadas antes de ejecutar cada instrucción (después de saltar a una excepción, si la hubo):

| Palabras | Contenido |
| --- | --- |
| 0-15 | GPR de a pares: `r(2k) \| r(2k+1) << 32` |
| 16-79 | FPR: ps0 y ps1 de cada registro (f0.ps0, f0.ps1, f1.ps0...) |
| 80 | CR \| XER << 32 |
| 81 | LR \| CTR << 32 |
| 82 | MSR \| FPSCR << 32 |
| 83 | SRR0 \| SRR1 << 32 |
| 84-87 | GQR0-7 de a pares |
| 88 | HID0 \| HID2 << 32 |

La huella del estado es una ronda tipo xxHash por palabra, desde 0: `h = rotl64(h + w × 0xC2B2AE3D27D4EB4F, 31) × 0x9E3779B185EBCA87`. La huella encadenada pasa por la misma ronda `pc | inst << 32` y la huella del estado de cada instrucción.

| Archivo | Contenido |
| --- | --- |
| `.huella` | 16 bytes por instrucción, little-endian: pc, instrucción, huella del estado |
| `.huella.detalle` | Para las instrucciones pedidas: índice (8 bytes), pc, instrucción y las 89 palabras |
| `.puntos` (y las referencias) | Texto: `instrucciones pc huella-encadenada` cada 65 536 instrucciones y al final |

Las huellas de 30 millones de instrucciones pesan unos 480 MB y quedan en `build/dolphin/trazas`; al repo van solo los puntos.

## Estado de GX (Fase 2, D-042)

La CPU no ve los registros de BP ni la memoria de XF, así que una traza igual no prueba que la GPU haya quedado igual. Al cortar la traza, el parche (con `GCPS3_ESTADO_GX=archivo`) y el runner (con `--estado-gx archivo`, después de atender los eventos de ese ciclo, como Dolphin en su `Advance`) escriben el mismo texto ([src/gx/estado.c](../src/gx/estado.c)):

| Línea | Qué es | En Dolphin |
| --- | --- | --- |
| `fifo base/end/hi/lo/distancia/escritura/lectura/bp` | El FIFO que lee la GPU | `SCPFifoStruct` del CP |
| `fifo banderas` | Lectura, breakpoint habilitado, su interrupción, breakpoint, interrupciones y estado de las marcas baja y alta, enlace (bits 0 a 8) | Los `bFF_*` |
| `pi base/end/escritura` | El FIFO del lado de la CPU | `ProcessorInterface` |
| `gp` | Bytes que quedaron en el gather pipe | `gather_pipe_ptr` |
| `bp`, `cp`, `xf` | Las palabras distintas de cero | `bpmem`, `FillCPMemoryArray`, `xfmem` |

`comparar.py` los compara después de la traza y con `--referencia` guarda el de Dolphin en `tests/dol/dolphin/<prueba>.gx.txt`; `test_dolphin.c` lo revisa cuando corre el DOL completo.

## Configuración de Dolphin

`comparar.py` arranca Dolphin igual que el runner arranca la GameCube (D-032, D-036): intérprete, sin hilo de CPU aparte, IPL salteado, DSP HLE, región NTSC-U e inglés (sin esto Dolphin toma el país del sistema, y sin país usa PAL), sin tarjetas de memoria, mando estándar en el puerto 1 siempre conectado (`GCPadNew.ini` con `Always Connected`; sin dispositivos de entrada figuraría desenchufado) y los otros vacíos, reloj fijo (`EnableCustomRTC`, el mismo `--rtc` que el runner), video `Null`, sin audio y sin límite de velocidad. El `.map` del DOL va a `Maps/<prueba>.map` del usuario de Dolphin, así Dolphin engancha los mismos `OSReport` (ganchos al inicio: no cambian el estado).

## Resultado (2026-10-23; `gx_fifo` el 2026-10-24)

Los 7 DOLs dan el mismo estado que Dolphin antes de cada instrucción, y el mismo estado de GX al final:

| DOL | Instrucciones comparadas | Qué cubre |
| --- | --- | --- |
| `hola` | 303 (completo) | Arranque HLE y ganchos |
| `cpu` | 673 867 (completo) | Enteros, FPU, paired singles, timebase |
| `timer` | 240 303 (completo) | 10 interrupciones del decrementador |
| `hw` | 4 991 377 (completo) | PI, SI, EXI, ARAM, DSP, DI, AI |
| `pantalla` | 30 000 000 | VI en 480i y el XFB |
| `contador` | 30 000 000 | 3 interrupciones de video |
| `gx_fifo` | 124 441 (completo) y el estado de GX | FIFO de GX, CP, PE (Fase 2, etapa A) |

Lo que hubo que cambiar para llegar ahí:

| Dónde | Qué | Cómo se vio |
| --- | --- | --- |
| gc-ps3 | El timebase arranca en la hora del reloj (`--rtc` × 40,5 MHz), no en 0 | `mftb` distinto en `timer` |
| gc-ps3 | El DOL empieza en el ciclo 20 000, el primer tramo de CoreTiming de Dolphin | Timebase 1 666 ticks adelante en Dolphin |
| gc-ps3 | Decrementador como Dolphin: cuenta desde que se escribe y queda en 0xFFFFFFFF al llegar a -1 | La excepción una instrucción antes en Dolphin |
| Parche de Dolphin | `fcmpu`/`fcmpo` no borraban el FPCC anterior (`FPRF & ~FPCC_MASK` con la máscara desplazada); los JIT sí | FPSCR 0xA000 en vez de 0x8000 en `cpu` |
| Parche de Dolphin | `addme`/`subfme` daban CA = 0 con CA = 1 a la entrada (`Helper_Carry(a, carry - 1)`); los JIT usan el acarreo de la máquina | XER distinto en `timer` |
| Configuración | Región NTSC-U, inglés y el mando siempre conectado | VI en PAL y el SI sin respuesta en `hw` |

Los dos errores del intérprete de Dolphin valen también para Dolphin sin parchar: se pueden informar al proyecto.

## Uso

```bash
make dolphin                 # una vez por sesión (~1 h con 2 núcleos)
make dolphin-ref             # compara los 7 DOLs y regenera tests/dol/dolphin
python3 tools/dolphin-trace/comparar.py tests/dol/bin/hw.dol          # uno solo
python3 tools/trace_diff.py build/dolphin/trazas/hw.runner.huella build/dolphin/trazas/hw.dolphin.huella
```

Si hay una diferencia, `comparar.py` repite las dos corridas con el detalle de esas instrucciones y muestra los registros distintos.
