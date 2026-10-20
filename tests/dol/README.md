# tests/dol

Programas de GameCube propios para probar el emulador de punta a punta, sin software de Nintendo y sin libc (D-034).

| Prueba | Qué prueba | Salida esperada |
| --- | --- | --- |
| `hola` | Arranque HLE, ganchos de `OSReport`, `vprintf`, `puts` y `exit`; el formateador con argumentos en registros, en la pila y en una `va_list` | La del mismo programa compilado en la PC |
| `cpu` | C compilado por GCC (enteros, 64 bits con libgcc, recursión, `switch`, `double` y `float`, conversiones) y lo propio del Gekko en ensamblador: paired singles, `psq_l`/`psq_st` con GQR, `fres`/`frsqrte`, el empate de `fmadds`, timebase y `sc` | La del mismo programa en la PC; las líneas del Gekko comparan contra valores conocidos e imprimen OK |
| `timer` | Manejador del decrementador copiado a 0x900, `MSR[EE]`, 10 interrupciones y el tiempo medido con el timebase | Escrita a mano |

## Archivos

| Archivo | Qué es |
| --- | --- |
| `crt0.S`, `link.ld` | Arranque (pila en 0x81700000, BSS en cero, `main`, `exit`) y enlace desde 0x80003100 |
| `rt.h`, `rt.c` | `OSReport`, `vprintf`, `puts`, `exit` con cuerpo vacío (el emulador los engancha por nombre en el `.map`), `memset`, `memcpy` |
| `rt_host.c` | Lo mismo para la PC, con la libc |
| `bin/*.dol`, `bin/*.map` | Lo compilado, versionado; el `.map` tiene el formato de Dolphin |
| `esperado/*.txt` | Lo que cada prueba tiene que imprimir |

## Cómo se usan

- `make test` (y `make test-be`) corre los tres DOLs dentro de las pruebas unitarias (`tests/unit/test_programs.c`): tienen que terminar con `exit(0)` e imprimir exactamente su `esperado/*.txt`.
- A mano: `build/host/gc-ps3-runner tests/dol/bin/cpu.dol` (toma solo el `.map` de al lado).
- `make testdols` los vuelve a compilar con `powerpc-linux-gnu-gcc -mcpu=750 -Wa,-m750cl` y regenera `esperado/hola.txt` y `esperado/cpu.txt` compilando las mismas fuentes para la PC (`-DGCPS3_HOST`). El resultado es el mismo byte a byte en cada compilación.

Las fuentes que también compilan en la PC evitan lo que cambia entre plataformas: `long` (32 bits en el Gekko, 64 en la PC), el signo de `char` y la contracción de `a*b+c` (`-ffp-contract=off` en los dos lados).
