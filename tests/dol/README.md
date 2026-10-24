# tests/dol

Programas de GameCube propios para probar el emulador de punta a punta, sin software de Nintendo y sin libc (D-034).

| Prueba | Qué prueba | Salida esperada |
| --- | --- | --- |
| `hola` | Arranque HLE, ganchos de `OSReport`, `vprintf`, `puts` y `exit`; el formateador con argumentos en registros, en la pila y en una `va_list` | La del mismo programa compilado en la PC |
| `cpu` | C compilado por GCC (enteros, 64 bits con libgcc, recursión, `switch`, `double` y `float`, conversiones) y lo propio del Gekko en ensamblador: paired singles, `psq_l`/`psq_st` con GQR, `fres`/`frsqrte`, el empate de `fmadds`, timebase y `sc` | La del mismo programa en la PC; las líneas del Gekko comparan contra valores conocidos e imprimen OK |
| `timer` | Manejador del decrementador copiado a 0x900, `MSR[EE]`, 10 interrupciones y el tiempo medido con el timebase | Escrita a mano |
| `pantalla` | VI en 480i NTSC programada a mano, "hola mundo" en el XFB con la fuente propia y barras de color; espera un cuadro entero e imprime el CRC-32 del XFB. Después se queda mostrando la imagen | La del mismo programa en la PC; además la prueba calcula el CRC del cuadro que recorrió la VI y tiene que ser el impreso |
| `contador` | Interrupción externa: manejador en 0x500 que borra DI0 y DI1, cuenta y anota el timebase; el número en pantalla avanza con cada campo y a las 9 interrupciones mide el período (675 675 ticks, 59,94 campos por segundo). Sigue contando | Escrita a mano |
| `gx_fifo` | Fase 2, etapa A, con la mini-librería `gxlib`: registros del CP, el PE y el PI al encender; BP (con la máscara), CP y XF por el gather pipe; un FIFO de 4 KB que da 6 vueltas regulado por la distancia; marcas alta y baja con su interrupción como el manejador de libogc; display lists; cargas indexadas de XF; primitivas (directas, indexadas, una de 640 bytes) y opcodes desconocidos; token con y sin interrupción, dos juntos, "draw done" y un token después de una lista larga; el breakpoint del FIFO; PI_FIFO_RESET. Manejador en 0x500 que anota y enmascara las causas del PI | La del runner revisada a mano; que es la de Dolphin lo prueba la traza, y el estado de BP, CP y XF se compara aparte |
| `gx_tri` | Fase 2, etapa B, con `gxlib` y `gxvtx`: cada tipo de primitiva con cantidades que sobran y que no alcanzan; la posición en cada formato (con y sin ByteDequant), indexada de 8 y 16 bits con vértices descartados y con otras matrices; el índice de matriz en el vértice; los colores en cada formato, indexados y solo el color 1; normales N, NTB con tres índices y float con las 8 luces, cada difusa y atenuación en los dos canales y la luz sobre un vértice; las 8 coordenadas de textura (cada fila, ST y STQ, AB11 y ABC1, emboss, desde los colores, q = 0, matriz final con y sin normalizar, matrices en el vértice, escala de BP); ortográfica; un lote de Dolphin con varias primitivas; la caché de normales; una display list; una tira de 150 vértices; XF y BP que no coinciden | La del runner (solo dice que la GPU terminó cada parte); lo que se compara son los 374 vértices transformados, bit a bit, contra el video por software de Dolphin |
| `gx_raster` | Fase 2, etapa C, con `gxlib` y `gxvtx`: recorte contra cada plano (lados, cerca, lejos, detrás de la cámara), contra varios a la vez, afuera del todo y degenerados; tira, abanico y cuadrados que cruzan los bordes; culling en sus 4 modos con los dos sentidos y el viewport al revés; líneas de varios anchos y pendientes, recortadas, con el desplazamiento de coordenadas; puntos con su tamaño y desplazamiento; sin detección de recorte; ortográfica. Desde la parte 7 (paso C3), con una ortográfica en píxeles exactos: relleno (Gouraud, bordes justo sobre las muestras en las tres rotaciones, astillas, abanico, líneas y puntos), tijera (impar, desplazada, la vuelta por 1024 con dos rectángulos que se pisan), Z (cada comparación, sin escritura, temprana y tardía con la prueba de alfa, zfreeze, recorte de Z), mezcla (los 64 pares de factores, resta, las 16 lógicas, alfa de destino, máscaras, dither, en RGB8 y RGBA6), los 8 formatos de píxel, la prueba de alfa, el TEV (registros, las 32 constantes, sesgo, escala, resta, comparaciones, dos etapas con swap) y la lectura del EFB por el CPU (color y Z, los modos de alfa del PE, 8, 16 y 64 bits) y la caja envolvente | La del runner (las partes y la suma de las lecturas del EFB y de la caja); lo que se compara son los 878 triángulos que llegan al rasterizador y el CRC del EFB en 44 copias sin destino, bit a bit, contra Dolphin |
| `hw` | Los registros a mano: revisión del PI, tipo de mando y sondeo por el SI, reloj y SRAM del IPL por EXI (con DMA) y su UART, 1 KB de ida y vuelta a la ARAM con ARINT, el primer correo del DSP, el DVD sin disco y el contador del AI. Solo imprime valores que no dependen de la hora | Escrita a mano |

## Archivos

| Archivo | Qué es |
| --- | --- |
| `crt0.S`, `link.ld` | Arranque (pila en 0x81700000, BSS en cero, `main`, `exit`) y enlace desde 0x80003100 |
| `rt.h`, `rt.c` | `OSReport`, `vprintf`, `puts`, `exit` con cuerpo vacío (el emulador los engancha por nombre en el `.map`), `memset`, `memcpy` |
| `rt_host.c` | Lo mismo para la PC, con la libc |
| `gxlib.h`, `gxlib.c` | Mini-librería de GX (D-041): el FIFO del CP y del PI, comandos al gather pipe (BP, CP, XF, indexadas, display lists, primitivas), espera leyendo SR, display lists en la RAM y `dcbf` |
| `gxvtx.c` | Lo de la mini-librería para vértices (etapa B, lo usan `gx_tri` y `gx_raster`): datos al gather pipe, VAT, arreglos, VCD con el INVTXSPEC de XF que compara Dolphin, índices de matriz, matrices, luces y proyección. En otro archivo para que `gx_fifo.dol` no cambie |
| `video.h`, `video.c` | VI en 480i NTSC, espera de cuadro, dibujo de a pares de píxeles en el XFB (YUV 4:2:2), fuente propia de 8x8 y CRC-32; en la PC el XFB es un arreglo |
| `bin/*.dol`, `bin/*.map` | Lo compilado, versionado; el `.map` tiene el formato de Dolphin |
| `esperado/*.txt` | Lo que cada prueba tiene que imprimir |
| `dolphin/*.txt` | La huella de Dolphin cada 65 536 instrucciones ([traza-dolphin](../../docs/traza-dolphin.md)); `make dolphin-ref` las regenera |
| `dolphin/*.gx.txt` | El estado de GX de Dolphin al final de la traza (FIFO, BP, CP y XF) |
| `dolphin/*.vtx.txt` | Cada vértice que transformó el video por software de Dolphin, en los DOLs que dibujan |
| `dolphin/*.tri.txt`, `dolphin/*.cop.txt` | Cada triángulo que llegó a su rasterizador y cada copia del EFB con el EFB al final |

## Cómo se usan

- `make test` (y `make test-be`) corre los DOLs dentro de las pruebas unitarias (`tests/unit/test_programs.c`) y cada uno tiene que imprimir exactamente su `esperado/*.txt`. `hola`, `cpu`, `timer`, `hw`, `gx_fifo`, `gx_tri` y `gx_raster` terminan con `exit(0)`; `pantalla` y `contador` no terminan y se corren 45 y 90 millones de ciclos.
- A mano: `build/host/gc-ps3-runner tests/dol/bin/cpu.dol` (toma solo el `.map` de al lado). Para los que no terminan, `--ciclos 100000000`. Con `--imagen archivo.png` guarda el último cuadro; el de `pantalla` es igual píxel por píxel al de la versión de PC (`build/testdols/host/pantalla imagen.ppm`), CRC-32 de los bytes RGB `0x10589e8c`.
- `make testdols` los vuelve a compilar con `powerpc-linux-gnu-gcc -mcpu=750 -Wa,-m750cl` y regenera `esperado/hola.txt`, `esperado/cpu.txt` y `esperado/pantalla.txt` compilando las mismas fuentes para la PC (`-DGCPS3_HOST`). El resultado es el mismo byte a byte en cada compilación.
- Para mirar el dibujo de `pantalla` sin el emulador: `build/testdols/host/pantalla imagen.ppm`.
- `make test` también corre cada DOL con la huella del estado de la CPU y la compara con la de Dolphin (`dolphin/*.txt`, `tests/unit/test_dolphin.c`): los que terminan, completos; `pantalla` y `contador`, 30 millones de instrucciones. Al final compara también el estado de GX (`dolphin/*.gx.txt`) y, mientras corre, cada vértice transformado, cada triángulo y cada copia (`dolphin/*.vtx.txt`, `*.tri.txt`, `*.cop.txt`).

Las fuentes que también compilan en la PC evitan lo que cambia entre plataformas: `long` (32 bits en el Gekko, 64 en la PC), el signo de `char` y la contracción de `a*b+c` (`-ffp-contract=off` en los dos lados).
