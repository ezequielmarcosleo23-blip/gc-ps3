# Fase 1

**Objetivo:** que un programa homebrew de GameCube hecho por nosotros corra en el intérprete y muestre su imagen, igual en el runner de PC y en la app de PS3 (en RPCS3 mientras no haya consola). Sin software de Nintendo. Plan aprobado el 2026-10-19.

## Etapas

| Etapa | Contenido | Se prueba con | Estado |
| --- | --- | --- | --- |
| A. Memoria y bus | 24 MB de RAM principal y 16 MB de ARAM con direccionamiento enmascarado (D-008); mapa de direcciones (0x80000000 con caché, 0xC0000000 sin caché); despacho de los registros de hardware ([mapa-memoria.md](mapa-memoria.md)) | Pruebas unitarias en PC, también en big-endian | Completa (2026-10-19): 12 pruebas nuevas, OK en x86-64 y en PowerPC 64 con QEMU |
| B. Intérprete de Gekko | Enteros, saltos, lectura y escritura de memoria, CR y XER, registros especiales; timebase, decrementador y excepciones; FPU escalar y paired singles en el PPE (D-007) | Oráculo (750CXe en QEMU), programas a mano y trazas de Dolphin | Completa (2026-10-19). B1: enteros y sistema, 11 578 casos del oráculo y 15 programas. B2: FPU escalar, paired singles y psq_l/psq_st, 21 326 casos del oráculo y 15 pruebas más (D-030, D-031). Todo OK en x86-64 y en PowerPC 64 |
| C. Arranque HLE | Cargador de DOL; estado inicial que deja el IPL (registros y variables de memoria baja); `OSReport` al log UDP; planificador de eventos por ciclos | Runner en PC | Completa (2026-10-19). C1: lector de DOL, planificador, arranque HLE ([arranque-hle.md](arranque-hle.md)) y runner con traza. C2: ganchos de depuración desde el `.map` (D-033) y tres DOLs propios en [tests/dol](../tests/dol/README.md) que imprimen lo mismo que en la PC. 14 pruebas nuevas, OK en x86-64 y en PowerPC 64 |
| D. Hardware mínimo | VI (búfer de imagen e interrupción de retrazo), PI (interrupciones), SI y EXI básicos; audio, disco y GX como stubs | DOLs de prueba | Completa (2026-10-19). D1: PI y VI ([src/hw](../src/hw/README.md)), DOLs `pantalla` y `contador` (D-035). D2: SI con mando estándar, EXI con reloj, SRAM y UART del IPL, DSP con buzones y DMA de la ARAM y de audio, AI, DVD sin disco, MI, CP, PE y FIFO de GX como stubs, DOL `hw` (D-036). 23 pruebas nuevas, OK en x86-64 y en PowerPC 64 |
| E. Imagen en PS3 | El mismo núcleo en la app; XFB (YUV422) a RGB en el PPU y en pantalla con rsxkit; en PC se guarda como imagen | RPCS3 y captura | En curso. E1 lista (2026-10-19): cuadro RGB con los dos campos (`src/core/xfb.c`), PNG del runner (`--imagen`) y CRC del cuadro (D-037); 4 pruebas nuevas. E2 lista en la PC (2026-10-20): app de PS3 con menú de DOLs, imagen con rsxkit y log de cuadros ([app-ps3.md](app-ps3.md)); `tools/compare_log.py` compara su log con el runner; `make test-app` corre la app simulada en la PC (D-038). En RPCS3 (2026-10-20) `contador` dio lo mismo que el runner (499 millones de ciclos, 61 cuadros con el mismo CRC), pero el menú no se movía y los DOLs salían repetidos con el paquete instalado: arreglado (palanca, repetición, botones en el log y en pantalla, sin repetidos); falta repetir la prueba con los demás DOLs. Falta E3 (traza de Dolphin y criterio de salida) |

## Criterio de salida

- Tres DOLs propios dan el mismo resultado en el runner y en RPCS3: un "hola mundo" en pantalla, una prueba de CPU que imprime OK y un contador que avanza con la interrupción de video.
- Los primeros millones de instrucciones de esos DOLs coinciden con una traza de Dolphin.

## Herramientas nuevas

- Etapa C: los DOLs de prueba se compilan con `powerpc-linux-gnu-gcc` (ya instalado para el oráculo), sin libogc (D-034).
- Etapa D: tampoco hace falta devkitPPC ni libogc; los DOLs de hardware siguen con el mismo compilador (D-035).
- Etapa E: nada nuevo para compilar. Para probar la app: RPCS3 con la red en "Connected" y `tools\logrecv.cmd` (Fase 0).

## Fuera de la Fase 1

Gráficos GX en el RSX, audio y DSP, lectura de ISOs de juegos y el JIT. Orden propuesto para después: GX, audio, JIT; se decide al cerrar esta fase.

## Reportes

Sin reportes técnicos por etapa: uno solo al terminar la Fase 1.
