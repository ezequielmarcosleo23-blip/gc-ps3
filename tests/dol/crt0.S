/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Arranque de los DOLs de prueba: pila, BSS en cero, main() y exit().
 * El IPL (o el arranque HLE) ya dejó MSR con FP, IR y DR, y HID2 con los
 * paired singles habilitados (docs/arranque-hle.md).
 */
    .section .init, "ax"
    .globl _start
_start:
    lis     1, _stack_top@ha
    addi    1, 1, _stack_top@l
    li      0, 0
    stwu    0, -16(1)               /* primer marco, cadena hacia atrás en 0 */

    lis     3, __bss_start@ha
    addi    3, 3, __bss_start@l
    lis     4, __bss_end@ha
    addi    4, 4, __bss_end@l
    li      5, 0
1:  cmplw   3, 4
    bge     2f
    stw     5, 0(3)
    addi    3, 3, 4
    b       1b

2:  bl      main
    bl      exit                    /* exit(valor de main) */
3:  b       3b

    .section .note.GNU-stack, "", @progbits     /* pila no ejecutable (aviso de ld) */
