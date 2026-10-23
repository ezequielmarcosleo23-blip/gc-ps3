# Arranque HLE de un DOL

Cómo deja `gc_system_boot_dol` (`src/core/system.c`) la máquina antes de la primera instrucción del programa. Son los valores que usa Dolphin para arrancar un ejecutable sin el IPL (commit `cf5cb7ef`: `Boot.cpp` y `Boot_BS2Emu.cpp`); el IPL de Nintendo no se usa (D-005, D-032).

## Orden

1. Se valida el DOL (`src/core/dol.c`); si está roto, nada cambia.
2. RAM en cero, CPU como recién encendida (`gekko_reset`) y planificador vacío.
3. Registros y memoria baja de las tablas de abajo.
4. Se copian las secciones del DOL (tamaño redondeado a 32 bytes si el archivo alcanza, como el IPL). El BSS no se borra: lo hace el crt0 del programa.
5. PC = entrada del DOL. Los GPR quedan en cero: el crt0 pone la pila y los punteros de datos chicos (r1, r2, r13).

## Registros

| Registro | Valor | Qué significa |
| --- | --- | --- |
| MSR | `0x00002032` | FP, IR, DR, RI: FPU habilitada y traducción de direcciones encendida; interrupciones (EE) apagadas |
| HID0 | `0x0011C464` | Cachés de instrucciones y datos, BHT, BTIC, DPM, NHR |
| HID1 | `0x80000000` | CPU a 3 veces el reloj del bus (de `gekko_reset`) |
| HID2 | `0xE0000000` | LSQE, WPE y PSE: `psq_l`/`psq_st`, write-gather y paired singles habilitados |
| IBAT0, DBAT0 | `0x80001FFF` / `0x00000002` | 0x80000000: 256 MB con caché |
| DBAT1 | `0xC0001FFF` / `0x0000002A` | 0xC0000000: 256 MB sin caché (registros de hardware) |
| PVR | `0x00083214` | Gekko 2.4e, el de las consolas de venta |
| ECID | `0x0D96E200`, `0x1840C00D`, `0x82BB08E8` | Identificador del chip (el de Dolphin) |
| FPSCR | 0 | Redondeo al más cercano |
| DEC | `0xFFFFFFFF` | Con el bit de arriba en 1 no baja ni hay excepción hasta que el programa escribe DEC, como en Dolphin (D-040) |
| Timebase | `--rtc` × 40 500 000 | Cuenta desde la hora del reloj y desde el ciclo 0, como Dolphin; el valor guardado de TBL/TBU es 0 hasta la primera lectura (D-040) |
| Ciclo | 20 000 | La primera instrucción del DOL corre en el ciclo 20 000: antes solo corre el hardware (el primer tramo de CoreTiming de Dolphin, D-040) |

Los BAT se guardan para quien los lea; la memoria emulada no traduce direcciones (D-008, D-026).

## Memoria baja

| Dirección | Valor | Qué es |
| --- | --- | --- |
| `0x80000020` | `0x0D15EA5E` | Arrancó desde el IPL |
| `0x80000028` | `0x01800000` | RAM física: 24 MB |
| `0x8000002C` | `0x10000006` | Tipo de consola: el último devkit. Dolphin usa este y no el de venta (3) porque con el de venta algunos juegos toman otros caminos de EXI |
| `0x800000CC` | 0 o 1 | Modo de video que dejó el IPL: NTSC o PAL (`--pal` en el runner) |
| `0x800000D0` | `0x01000000` | ARAM: 16 MB |
| `0x800000F8` | `162000000` | Reloj del bus (Hz) |
| `0x800000FC` | `486000000` | Reloj de la CPU (Hz) |
| `0x800030D8` | segundos × 40 500 000 | Hora del sistema en ticks de timebase desde el 2000-01-01 (`--rtc` en el runner; por omisión la de la PC) |

En los vectores de excepción `0x100`, `0x200`, `0x300`, `0x400`, `0x500`, `0x600`, `0x700`, `0x800`, `0x900`, `0xC00`, `0xD00`, `0xF00`, `0x1300`, `0x1400` y `0x1700` queda un `rfi`: una excepción antes de que el programa instale sus manejadores vuelve sin romper nada.

## Hardware

Todo el hardware queda con los valores de encendido de Dolphin ([src/hw](../src/hw/README.md), D-036):

| Bloque | Cómo queda |
| --- | --- |
| PI | Causa VI prendida (hasta la primera media línea), máscara en cero |
| VI | Tiempos de `Preset`: NTSC a 54 MHz o PAL a 27 MHz según `--pal`; la primera media línea agendada |
| SI | Mando estándar en el puerto 1, sin botones; puertos 2 a 4 vacíos; SIPOLL con X = 492 |
| EXI | Chip del IPL: reloj = `--rtc` más el tiempo emulado, SRAM de Dolphin (inglés, estéreo); ranuras de tarjeta vacías |
| DSP | Detenido con DSPInit en 1; su primer correo (0x8071FEED) espera a que arranque. ARAM de 16 MB en cero |
| AI | Parado; AIS a 48 kHz y AID a 32 kHz |
| DI | Sin disco: tapa abierta |

## Después del arranque

Si hay un `.map` (el runner busca el que está al lado del DOL), se enganchan las funciones de depuración conocidas (`src/core/hle_os.c`): lo que imprimen va al log y `exit` detiene la CPU (D-033).

## Pendiente para etapas siguientes

- Variables que solo pone el apploader de un disco (ID del juego en `0x80000000`, FST, arena): cuando se arranquen ISOs, fuera de la Fase 1.
