# Presupuesto de memoria

Cuánta memoria de la PS3 usa cada parte del emulador. Se completa con mediciones reales; lo que aún no se ha medido queda como "por medir".

## Lo que hay que emular (GameCube)

| Región | Tamaño |
| --- | --- |
| MEM1 (RAM principal, 1T-SRAM) | 24 MB |
| ARAM (memoria auxiliar) | 16 MB |
| EFB y caché de texturas dentro de Flipper | ~3 MB |

## Dónde vive en la PS3

| Uso | Memoria | Tamaño | Estado |
| --- | --- | --- | --- |
| MEM1 emulada | XDR (principal) | 24 MB | por medir disponibilidad en el proceso |
| ARAM emulada | XDR | 16 MB | por medir |
| Caché de código del JIT (cola NOBITS) | XDR | por definir en la prueba `jit` | por medir |
| Búferes de la ISO (streaming) | XDR | por definir en la prueba `iso` | por medir |
| Framebuffers, texturas y programas del RSX | GDDR3 (local del RSX) | por definir en la prueba `rsx` | por medir |
| Local store de cada SPE | 256 KB por SPE | código + datos + doble búfer DMA | por medir en la prueba `spu` |
