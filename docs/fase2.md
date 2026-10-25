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
| B. Vértices y XF | Formatos de vértice (VAT/VCD, directo e índices de 8 y 16 bits), tipos de componente y de color, matrices indexadas, proyección, normales, coordenadas de textura, 8 luces | Unitarias con valores calculados; DOL `gx_tri` contra los vértices del video por software de Dolphin | Completa (2026-10-24), ver abajo |
| C. Rasterizador de referencia | Viewport, primitivas, recorte, culling, Z en los formatos del EFB, scissor, TEV en enteros como el hardware, alpha test, mezcla, todos los formatos de textura con paleta y filtros, copias del EFB a la RAM (XFB y texturas). Corre también en la PS3 hasta que dibuje el RSX (cambia el punto 1 de D-041) | DOLs `gx_raster`, `gx_tex`, `gx_tev`, `gx_copy`, `gx_escena`; cada triángulo, el EFB y cada copia contra el renderizador por software de Dolphin | Completa (2026-10-24), ver abajo |
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

## Etapa B: resultado (2026-10-24)

El viewport pasó a la etapa C: va después del recorte, y en el video por software de Dolphin también (`Clipper`), así que la comparación de vértices termina en la posición proyectada.

| Paso | Qué quedó |
| --- | --- |
| B1. Referencia | Dolphin con video por software sin ventana: el parche anota cada vértice que transforma (`GCPS3_VERTICES`) y, en la traza, toma el reloj del tiempo emulado. Las referencias de los 7 DOLs no cambiaron con este video ([traza-dolphin.md](traza-dolphin.md)) |
| B2. Cargador de vértices | Juntar en el PPU (índices a bytes de la RAM, descartes) y decodificar listo para SPU en bloques de 64: los 5 formatos de componente, ByteDequant, los 6 de color, NTB con uno y tres índices, índices de matriz ([src/gx/vtx.c](../src/gx/vtx.c)). La semántica es la del cargador x64 de Dolphin, el que usa por omisión |
| B3. XF | Posición, perspectiva y ortográfica, normales, color y alfa en los dos canales con las 8 luces (difusa y atenuación de cada tipo), coordenadas de textura (filas, emboss, colores, matriz final, q = 0, escala de BP), listo para SPU con una foto del XF de 7 KB ([src/gx/xform.c](../src/gx/xform.c)). Las mismas operaciones de float en el mismo orden que `TransformUnit`, sin FMA |
| B4. Primitivas | Cada primitiva se carga y se transforma al leerla: partes de 16 380, los vértices que usa cada tipo, la caché de normales, XF y BP que no coinciden ([src/gx/prim.c](../src/gx/prim.c)); `--vertices` en el runner y la comparación en `comparar.py` y en `test_dolphin.c` |
| B5. DOL | Funciones de vértices en `tests/dol/gxvtx.c` (aparte, para que `gx_fifo.dol` no cambie) y DOL `gx_tri` (12 partes, [tests/dol](../tests/dol/README.md)): igual a Dolphin en sus 136 905 instrucciones, el estado de GX y los 374 vértices, bit a bit, en la primera corrida |
| B6. Verificación | 28 pruebas unitarias nuevas y 2 de DOL (158 en total), OK en x86-64 con ASan/UBSan y en PowerPC 64; `make test-app` corre también `gx_tri`. 24 de 25 mutaciones detectadas (la que no: el orden de una multiplicación del alfa, que el redondeo a 8 bits tapa). Compilado con FMA (`-ffp-contract=fast -O2` en PowerPC) los vértices de `gx_tri` dejan de ser los de Dolphin. Decisión D-043 |

Dolphin transforma al vaciar el lote, con el estado de ese momento; gc-ps3, al leer la primitiva, como el hardware. Da lo mismo porque todo lo que cambia el XF vacía el lote antes, salvo escribir XF 0x1018 o 0x1019 con el mismo valor que ya tiene el registro de CP (MATINDEX); libogc escribe primero el de CP, que sí lo vacía.

## Etapa C: plan y avance

Plan aprobado el 2026-10-24: se porta el renderizador por software de Dolphin y se compara cada paso; un commit cada uno o dos pasos.

| Paso | Contenido | Estado |
| --- | --- | --- |
| C1. Referencia | El parche anota cada copia del EFB (`GCPS3_COPIAS`: parámetros, CRC del EFB y de lo escrito en la RAM; al final, el EFB entero) y cada triángulo que llega al rasterizador (`GCPS3_TRIANGULOS`). El runner da lo mismo con `--copias` y `--triangulos`; `comparar.py` y `test_dolphin` los comparan. EFB de 640 × 528 con los formatos de píxel y el borrado al copiar ([src/gx/efb.c](../src/gx/efb.c), [copy.c](../src/gx/copy.c)) | Completo (2026-10-24) |
| C2. Ensamblado y recorte | El orden de IndexGenerator, recorte contra los 6 planos con la interpolación de Dolphin, culling, división por w y viewport, líneas y puntos como triángulos con su ancho y el desplazamiento de las coordenadas ([src/gx/setup.c](../src/gx/setup.c)). DOL `gx_raster`: los 160 triángulos iguales a Dolphin, bit a bit | Completo (2026-10-24) |
| C3. Rasterizador y EFB | Rectángulos de tijera (con la vuelta por 1024), aristas en 28.4 con la convención de relleno, bloques de 2 × 2, pendientes de Z, 1/w, colores y coordenadas, LOD, zfreeze ([src/gx/raster.c](../src/gx/raster.c)); el TEV entero salvo el muestreo de texturas, que da negro hasta C4 ([tev.c](../src/gx/tev.c)); Z temprana y tardía, mezcla, resta, lógicas, alfa de destino, máscaras y dither ([efb.c](../src/gx/efb.c)); la caja envolvente ([bbox.c](../src/gx/bbox.c)) y la lectura del EFB por el CPU en 0xC8000000. DOL `gx_raster`, partes 7 a 12: igual a Dolphin en sus 252 690 instrucciones (con las lecturas del EFB y de la caja), 1 649 vértices, 878 triángulos y 44 copias con el CRC del EFB. 17 de 20 mutaciones detectadas (ver abajo) | Completo (2026-10-24) |
| C4. Texturas | Decodificadores de los 11 formatos con las 3 paletas, listos para SPU, por texel y la textura entera de a bloques (iguales entre sí) ([src/gx/texdec.c](../src/gx/texdec.c)); TMEM con la carga de paletas y la precarga, también RGBA8 en dos bancos, y el muestreador: envolturas, más cercano y bilineal, mipmaps más cercano y lineal con el LOD de C3 ([tex.c](../src/gx/tex.c)). DOL `gx_tex`: igual a Dolphin en sus 611 772 instrucciones, 416 vértices, 210 triángulos y 17 copias con el CRC del EFB, desde la primera corrida. 21 de 21 mutaciones detectadas (entre ellas el sesgo del LOD que `gx_raster` no veía). El runner deja el EFB final en PNG con `--efb` | Completo (2026-10-24) |
| C5. TEV | Revisado contra Tev.cpp con las texturas de C4. La niebla ya no usa `pow` de la libm: `gx_exp2` ([src/gx/exp2.c](../src/gx/exp2.c)) calcula 2^x en double-double con suma, resta, multiplicación y división, así que da lo mismo en la PC y en el PPU. Comparado con la glibc 2.39 para los 1 090 519 041 floats de [-8, 0] ([tools/exp2/comprobar.c](../tools/exp2/comprobar.c)): el float de los 4 tipos exponenciales y `fogInt` iguales en todos, con la variante FMA de `pow` y sin ella. En double difiere en 111 791 casos (134 106 sin FMA): son los que glibc redondea mal, a menos de una millonésima de ULP del medio. DOL `gx_tev`: 320 configuraciones al azar de 1 a 16 etapas (combinadores, constantes, swap, texturas, indirectas, prueba de alfa, Z, textura de Z, niebla) y partes dirigidas (indirectas con texturas armadas, textura de Z con lecturas del CPU, los 8 tipos de niebla, el caso NaN y el rango, las 256 pruebas de alfa, las comparaciones y el recorte a -1024); igual a Dolphin en sus 2 565 809 instrucciones, 2 180 vértices, 802 triángulos y 447 copias con el CRC del EFB, desde la primera corrida. 53 de 54 mutaciones detectadas (la que no, ver abajo) | Completo (2026-10-24) |
| C6. Copias del EFB | La referencia: con el video por software Dolphin escribe cada copia en la RAM en el momento (no le cambian los "hacks" de copias a textura ni las copias diferidas), pero por omisión apaga el filtro vertical de la copia al XFB: `comparar.py` lo prende (`DisableCopyFilter = False`); las 11 referencias anteriores no cambiaron. A textura: los 13 formatos desde RGB8, RGBA6, RGB565 y la Z, intensidad y media escala, listos para SPU ([src/gx/texenc.c](../src/gx/texenc.c)). Al XFB: filtro vertical, recortes, gamma, YUV 4:2:2 y escala vertical ([xfbenc.c](../src/gx/xfbenc.c)); la gamma es una tabla de 4 × 256 que sale de `powf` de glibc, igual con FMA y sin FMA ([tools/gamma/tabla.c](../tools/gamma/tabla.c)). Las dos pasan por el búfer intermedio de Dolphin, que queda entre copias ([copy.c](../src/gx/copy.c)). DOL `gx_copy`: igual a Dolphin en sus 32 203 927 instrucciones, 696 vértices, 240 triángulos y 183 copias (162 con el CRC de lo escrito en la RAM), desde la primera corrida; la escena copiada al XFB sale en pantalla por la VI. 36 de 36 mutaciones detectadas | Completo (2026-10-24) |
| C7. Verificación | DOL `gx_escena`: 4 cuadros de una escena con piso iluminado y con textura de 6 mipmaps, un cubo que gira con 2 etapas del TEV, un espejo que es una copia del EFB a textura dibujada antes en el mismo cuadro, un estanque con textura indirecta y mezcla, un vidrio con el alfa de los vértices y niebla; cada cuadro va al XFB y sale por la VI. Igual a Dolphin en sus 129 423 913 instrucciones, 640 vértices, 325 triángulos y 10 copias, desde la primera corrida. Tiempo por cuadro: la GPU emulada cuenta el tiempo que corre y las copias al XFB; el runner lo anota al terminar y la app también (`app: GX de ...`). En la PC (x86-64, `-O2`): 73 ms por cuadro de `gx_escena`; la cifra del PPU queda para la consola ([perf-log.md](perf-log.md)). Las 13 referencias regeneradas con `make dolphin-ref` no cambiaron. Las 130 mutaciones de C3 a C6 juntas contra los 5 DOLs que dibujan: 127 detectadas (`gx_escena` detecta 24); las 3 que no son las ya explicadas (dos de la tijera y `K[5]`). 199 pruebas OK en x86-64 con ASan/UBSan y en PowerPC 64, `make test-app` con 12 corridas, y el build de PS3. Decisión D-044 | Completo (2026-10-24) |

Con un lote de Dolphin, lo que no lo vacía se aplica a todo el lote al dibujarlo: además de XF 0x1018/0x1019 (etapa B), XF 0x1005 (sin detección de recorte). Los DOLs vacían el lote antes de volver a cambiarlo.

Lo que se sigue de Dolphin aunque el hardware sea otro (D-044):

- La referencia corre con `EFBAccessEnable` y `BBoxEnable` prendidos (Dolphin los apaga por omisión para ir más rápido; los juegos que los necesitan los prenden en su INI). `comparar.py` los pone en el `GFX.ini` del usuario: con `-C` no se aplican.
- La caja envolvente tiene la caché de VideoCommon: escribir un valor igual al que guarda no lo marca, así que el 1023 0 1023 0 del SDK no vuelve a 0 la derecha y abajo si ya eran 0, y queda lo dibujado antes. Después de leerla queda apagada hasta la próxima escritura.
- Los contadores de rendimiento del PE dan 0 (`PerfQueriesEnable` apagado; la versión de Dolphin cuenta uno de cada 3 píxeles).
- El TEV es un objeto estático: el color de un canal que no está activo, la textura de una etapa sin textura y la coordenada de una que no existe quedan del píxel anterior.
- La niebla de rango lee `K[5]`, que cae en el registro de la niebla `a` (BP 0xEE). Solo pasa con el índice justo en 9 y entonces su peso es 0: no se ve nunca (es la mutación que `gx_tev` no detecta).
- La niebla usa `pow` y `sqrt` de double de la libm de la PC (`::pow`, no `powf`). `sqrt` es igual en cualquier libm; para `pow` vale el float de glibc, que `gx_exp2` reproduce.
- Los rectángulos de tijera se recalculan al dibujar si cambió BP 0x20, 0x21, 0x59 o el viewport, y después de una copia con borrado; antes del primer cambio no hay ninguno y no se dibuja nada.

- Texturas: el comienzo de cada mipmap se cuenta con el tamaño sin redondear al bloque; en RGBA8 precargado al bajar de mipmap solo avanza el banco par; repetir hace AND con el tamaño - 1 también sin potencia de 2; un formato de textura que no existe da 0 (Dolphin deja lo que había en la pila).

- Copias: el relleno de los bloques lee de corrido el EFB de Dolphin (la fila siguiente, y debajo del color, la Z); la cantidad de bloques sale del tamaño pedido sin recortar al EFB; con un paso menor que la fila, cada fila lleva solo lo que entra en el paso (la imagen sale mezclada); desde la Z a media escala, RGBA8, RG8 y GB8 llevan los bytes en otro orden que sin escala; RGB5A3 a media escala corta en alfa 224.
- Copia al XFB: escribe los pares Y/UV en un búfer que queda entre copias en la x del EFB y lee desde 0 (con el rectángulo corrido a la derecha se ve una copia anterior); la escala vertical lee la fila round(y × alto / alto escalado) con un `round` de double, y si el XFB tiene más filas que el alto escalado, o el paso es mayor que la fila, sale lo que quedaba en el búfer intermedio.
- El generador de shaders de Dolphin corre aunque el video sea por software y tiene sus ASSERT: una matriz indirecta dinámica (S o T) en una etapa cuya coordenada no existe, con su etapa indirecta activa, lo detiene. Los DOLs no lo usan (en el hardware no está definido).

Las mutaciones que `gx_raster` no detecta: el desempate de los rectángulos de tijera por área (solo se nota dibujando fuera del viewport) y el recálculo de la tijera después de borrar (ya está al día por los cambios). El sesgo del LOD lo detecta `gx_tex`.

## Criterio de salida

1. Los DOLs de GX dan en el runner el mismo XFB que el renderizador por software de Dolphin (byte a byte donde se pueda; si no, con la tolerancia anotada).
2. Los mismos DOLs en RPCS3 (RSX) quedan dentro de una tolerancia acordada respecto del runner (el TEV calcula en enteros de 10 bits y el RSX en fp16/fp32: no pueden ser idénticos).
3. La traza de la CPU sigue igual a la de Dolphin en todos los DOLs (D-039).

## Decisiones (D-041)

| # | Decisión |
| --- | --- |
| 1 | Un rasterizador por software propio, solo para la PC: referencia, pruebas en el CI y depuración. No va en la PS3 (cambiado por D-044: corre también en el PPU hasta que dibuje el RSX) |
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
| En las SPU el float redondea hacia cero y no tiene denormales: el XF ahí no puede dar los bits de Dolphin | Decidirlo en la fase de rendimiento (doble precisión, tolerancia, o quedarse en el PPU); el PPU es la referencia |
| Es la fase más grande | Etapas cortas con pruebas, plan de cada etapa antes de empezarla |

## Fuera de la Fase 2

Juegos e ISOs, audio y DSP, el JIT, las SPU y la optimización.

## Reportes

Sin reportes técnicos por etapa.
