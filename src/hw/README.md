# src/hw

El hardware de la GameCube alrededor de la CPU: cada bloque registra sus registros en el mapa de MMIO (`src/core/mmio.h`), avisa sus interrupciones al PI y, si necesita tiempo, agenda eventos en el planificador. Sigue la semántica de Dolphin (commit `cf5cb7ef`); lo portado lleva la atribución en el encabezado.

| Archivo | Bloque | Qué hace |
| --- | --- | --- |
| `pi.h/.c` | Processor Interface (`0x0C003000`) | Junta las interrupciones de todo el hardware (INTSR, se borra escribiendo 1) y le maneja a la CPU la línea externa según la máscara (INTMR). Revisión de Flipper, código de reset, registros del FIFO de GX |
| `vi.h/.c` | Video Interface (`0x0C002000`) | Registros de 16 bits, un evento por media línea, posición del haz (DPV, DPH), interrupciones DI0-DI3, el aviso de cada campo terminado con su XFB y el momento de sondear los mandos |
| `si.h/.c` | Serial Interface (`0x0C006400`) | 4 puertos: comandos por el búfer de 128 bytes (estado, origen, lectura directa), sondeo cada X líneas con IN_HI/IN_LO y RDST, SIOUT para el modo del mando. Mando estándar en el puerto 1, el resto vacío |
| `exi.h/.c` | External Interface (`0x0C006800`) | 3 canales, transferencias inmediatas y por DMA. Chip del IPL en el canal 0 (CS 2): reloj, SRAM con los valores de Dolphin y la UART de depuración (su texto sale como OSReport). Ranuras de tarjeta vacías, sin ROM del IPL |
| `dsp.h/.c` | Interfaz del DSP (`0x0C005000`) | DSPCSR, buzones con los correos que da Dolphin por HLE antes de cargar un microcódigo (0x8071FEED, 0x80544348, 0xFEEExxxx), DMA de la ARAM con su interrupción y DMA de audio por bloques con AID |
| `ai.h/.c` | Audio Interface (`0x0C006C00`) | Contador de muestras a 48 o 32 kHz con PSTAT, interrupción al llegar a AIIT, frecuencias de AIS y AID |
| `di.h/.c` | DVD Interface (`0x0C006000`) | Sin disco: tapa abierta; Inquiry y RequestError contestan, las lecturas dan error de "sin medio"; cada comando tarda 300 µs |
| `mi.h/.c` | Memory Interface (`0x0C004000`) | Registros de 16 bits que guardan lo escrito |
| `gx.h/.c` | CP (`0x0C000000`), PE (`0x0C001000`) y FIFO de GX (`0x0C008000`) | Registros guardados con las máscaras de Dolphin; CP siempre ocioso; lo escrito en el FIFO se cuenta y se descarta hasta que haya GX |

## Cómo se arma

`gc_system_init` registra los bloques una vez; `gc_system_hle_boot` los deja con los valores de encendido de Dolphin (PI con la causa VI prendida, VI con los tiempos de `Preset`, AI a 48/32 kHz, DSP detenido con DSPInit, DVD con la tapa abierta) y agenda la primera media línea y el DMA de audio. El campo que termina se informa con `gc_vi_field_t` (dirección, ancho, separación entre líneas y alto) y la VI guarda el último de cada paridad: con esos dos `src/core/xfb.c` arma el cuadro RGB que guarda el runner (`--imagen`) y que va a mostrar la app de PS3. El estado del mando está en `gc_si_t.pad`: la plataforma lo va a llenar cuando haya entrada.

## Tiempos

| Qué | Ciclos de CPU |
| --- | --- |
| Media línea de VI, encendido NTSC (54 MHz, 525 por campo, 119,88 campos/s) | 7 722 |
| Media línea de VI, 480i NTSC (27 MHz, 525 por campo, 59,94 campos/s) | 15 444 |
| Sondeo de mandos | 15 medias líneas después del comienzo de cada campo, luego cada X líneas |
| Muestra del AI a 48 kHz / 32 kHz | 10 116 / 15 174 |
| Bloque de 32 bytes del DMA de audio a 32 kHz / 48 kHz | 121 392 / 80 928 |
| Bloque de 32 bytes del DMA de la ARAM | 246 |
| Comando del DVD | 145 800 (300 µs) |
| DSPInitCode después de bajar DSPInit | 130 ticks de timebase |

Ciclos por media línea = 2 × 486 MHz / reloj de VI × HLW (429). Un campo de 480i son 8 108 100 ciclos: 675 675 ticks de timebase.

## Diferencias con Dolphin

- Si HLW vale 0, Dolphin divide por cero; acá la media línea dura 64 ciclos.
- El reloj del IPL cuenta desde la hora de arranque (`--rtc` en el runner) más el tiempo emulado, no desde el reloj de la PC en cada lectura: la emulación da siempre lo mismo.
- Con el AI parado, el contador de muestras queda quieto. Dolphin le suma el ciclo en que paró, un error que no se copia.
- Un comando del SI que el mando no termina (0x1D o uno desconocido) queda en curso; Dolphin lo reintenta sin fin en el mismo ciclo.
- La ROM del IPL no está (D-005): sus lecturas dan 0, sin el texto de copyright ni las fuentes.
- El DSP no carga microcódigos (llega con el audio); sin ajuste de velocidad, sin salida temprana del XFB ni dispositivos de Triforce o Wii.

## Pruebas

`tests/unit/test_hw.c` (PI y VI), `tests/unit/test_hw_io.c` (SI, EXI, DSP y ARAM, AI, DI, MI, CP, PE y FIFO) y los DOLs `pantalla`, `contador` y `hw` de [tests/dol](../../tests/dol/README.md).
