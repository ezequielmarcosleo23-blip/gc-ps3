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
| `fpu_util.h/.c` | Aritmética del Gekko portada de Dolphin: NaN y FPSCR, Force25Bit, fmadd de simple con un solo redondeo, conversiones de lfs/stfs/psq, tablas de fres y frsqrte, modo de redondeo de la máquina |
| `interp_fpu.c` | FPU escalar (opcodes 59 y 63), `fctiw`, comparaciones, FPSCR (`mffs`, `mtfsf`...), `lfs`/`lfd`/`stfs`/`stfd`/`stfiwx` |
| `interp_ps.c` | Paired singles (opcode 4) y `psq_l`/`psq_st` con GQR (float, u8, s8, u16, s16) |

## Cómo se verifica

- **Oráculo:** `tools/ppc-oracle` ejecuta cada instrucción de enteros en un PowerPC 750CXe emulado por QEMU (la base de Gekko) y guarda 11 578 casos en `tests/unit/data/oracle_int.bin`. `tests/unit/test_cpu_int.c` los pasa por el intérprete y compara r3, r4, XER y CR. `make oracle` regenera las tablas (necesita `gcc-powerpc-linux-gnu` y `qemu-user`).
- **Oráculo de la FPU:** `tools/ppc-oracle/oracle_fpu.c` hace lo mismo con la FPU escalar: 21 326 casos en `oracle_fpu.bin` (los 4 modos de redondeo, NaN, subnormales, bordes de simple y de enteros, empates de fmadds, cargas y almacenamientos, instrucciones de FPSCR). `test_cpu_fpu.c` compara f1, FPSCR y CR; cada fila marca los bits que no se comparan (ver la cabecera de `oracle_fpu.c`).
- **Paired singles** (`test_cpu_ps.c`): el 750CXe no los tiene; cada mitad se compara con la escalar de simple equivalente en los 4 modos, y la cuantización, las mezclas y HID2 van a mano.
- **Programas cortos a mano** (`tests/unit/test_cpu.c`): saltos, memoria, reservas, cadenas, caché bloqueada, SPR, excepciones, decrementador e interrupción externa.
- Todo corre en x86-64 con ASan/UBSan y en big-endian con `make test-be`.
- `fpu_util.c` se comparó una vez contra el código de Dolphin compilado aparte (28,8 millones de operaciones al azar en los 4 modos): mismos bits salvo los dos casos de D-030.

## Decisiones de semántica

- Cada instrucción cuenta 1 ciclo; el timebase avanza 1 cada 12 ciclos y el decrementador con él.
- `divw` y `divwu` con resultado indefinido (divisor 0, `0x80000000 / -1`): 0xFFFFFFFF si el dividendo es negativo y 0 si no, como Dolphin.
- Las operaciones de caché (`dcbf`, `dcbst`, `dcbi`, `icbi`...) no hacen nada en el intérprete; `dcbz` y `dcbz_l` sí ponen a cero.
- Sin comprobación de privilegios: los juegos corren en modo supervisor.
- FPU (D-030, D-031): las de simple dejan el resultado en ps0 y ps1, las dobles solo en ps0; fmuls, fmadds y los paired singles redondean frC a 25 bits; `fres`/`frsqrte` usan las tablas del Gekko; `fctiw` deja 0xFFF80000 en la palabra alta. FI, FR, OX, UX y XX no siempre se calculan (como en Dolphin).
- MSR[FP] = 0 da "FPU no disponible"; HID2[PSE] = 0 hace ilegales los paired singles y `psq_l`/`psq_st` piden además HID2[LSQE]. Una dirección no múltiplo de 4 en `lfs`/`stfd`... da excepción de alineación.
- Con FPSCR[FEX] y MSR[FE0|FE1], excepción de programa con SRR0 = la instrucción.
