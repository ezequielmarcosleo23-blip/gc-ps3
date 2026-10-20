# src/hw

El hardware de la GameCube alrededor de la CPU: cada bloque registra sus registros en el mapa de MMIO (`src/core/mmio.h`), avisa sus interrupciones al PI y, si necesita tiempo, agenda eventos en el planificador. Sigue la semántica de Dolphin (commit `cf5cb7ef`); lo portado lleva la atribución en el encabezado.

| Archivo | Bloque | Qué hace |
| --- | --- | --- |
| `pi.h/.c` | Processor Interface (`0x0C003000`) | Junta las interrupciones de todo el hardware (INTSR, se borra escribiendo 1) y le maneja a la CPU la línea externa según la máscara (INTMR). Revisión de Flipper, código de reset, registros del FIFO de GX |
| `vi.h/.c` | Video Interface (`0x0C002000`) | Registros de 16 bits, un evento por media línea, posición del haz (DPV, DPH), interrupciones DI0-DI3 y el aviso de cada campo terminado con su XFB |

## Cómo se arma

`gc_system_init` registra los bloques una vez; `gc_system_hle_boot` los deja con los valores de encendido de Dolphin (PI con la causa VI prendida, VI con los tiempos de `Preset`) y agenda la primera media línea. El campo que termina se informa con `gc_vi_field_t` (dirección, ancho, separación entre líneas y alto): el runner lo muestra al final y la etapa E lo va a pasar a RGB.

## Tiempos de la VI

| Modo | Reloj | Ciclos de CPU por media línea | Medias líneas por campo | Campos por segundo |
| --- | --- | --- | --- | --- |
| Encendido NTSC (Dolphin) | 54 MHz | 7 722 | 525 | 119,88 |
| 480i NTSC (SDK) | 27 MHz | 15 444 | 525 | 59,94 |

Ciclos por media línea = 2 × 486 MHz / reloj × HLW (429). Un campo de 480i son 8 108 100 ciclos: 675 675 ticks de timebase.

## Diferencias con Dolphin

- Si HLW vale 0, Dolphin divide por cero; acá la media línea dura 64 ciclos.
- Sin sondeo de mandos en la VI, ni ajuste de velocidad, ni salida temprana del XFB: llegan con SI (etapa D2) y la imagen (etapa E).

## Pruebas

`tests/unit/test_hw.c` (registros, tiempos, interrupciones hasta la CPU, campos) y los DOLs `pantalla` y `contador` de [tests/dol](../../tests/dol/README.md).
