# src/cpu

Intérprete de Gekko (Fase 1, etapa B). El mismo código corre en la PC y en el PPE; el JIT llega después.

| Archivo | Qué hace |
| --- | --- |
| `gekko.h` | Estado de la CPU (GPR, FPR como pares ps0/ps1, CR, XER, LR, CTR, MSR, SPR), constantes y API |
| `gekko.c` | Tablas de decodificación, paso y bucle, excepciones, timebase, decrementador, SPR con efectos (DMA de la caché bloqueada) |
| `interp.h` | Campos de la instrucción y ayudas de CR/XER (interno) |
| `interp_int.c` | Aritmética, lógica, comparaciones, rotaciones, desplazamientos, trap |
| `interp_branch.c` | `b`, `bc`, `bclr`, `bcctr`, lógica de CR, `mcrf` |
| `interp_mem.c` | Lecturas y escrituras (D y X, con actualización), `lmw`/`stmw`, cadenas, bytes invertidos, `lwarx`/`stwcx.`, `dcbz`, `dcbz_l` |
| `interp_sys.c` | `mfspr`/`mtspr`, `mftb`, MSR, CR completo, segmentos, `sc`, `rfi` |

## Cómo se verifica

- **Oráculo:** `tools/ppc-oracle` ejecuta cada instrucción de enteros en un PowerPC 750CXe emulado por QEMU (la base de Gekko) y guarda 11 578 casos en `tests/unit/data/oracle_int.bin`. `tests/unit/test_cpu_int.c` los pasa por el intérprete y compara r3, r4, XER y CR. `make oracle` regenera las tablas (necesita `gcc-powerpc-linux-gnu` y `qemu-user`).
- **Programas cortos a mano** (`tests/unit/test_cpu.c`): saltos, memoria, reservas, cadenas, caché bloqueada, SPR, excepciones, decrementador e interrupción externa.
- Las dos corren en x86-64 con ASan/UBSan y en big-endian con `make test-be`.

## Decisiones de semántica

- Cada instrucción cuenta 1 ciclo; el timebase avanza 1 cada 12 ciclos y el decrementador con él.
- `divw` y `divwu` con resultado indefinido (divisor 0, `0x80000000 / -1`): 0xFFFFFFFF si el dividendo es negativo y 0 si no, como Dolphin.
- Las operaciones de caché (`dcbf`, `dcbst`, `dcbi`, `icbi`...) no hacen nada en el intérprete; `dcbz` y `dcbz_l` sí ponen a cero.
- Sin comprobación de privilegios: los juegos corren en modo supervisor.
