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
 * Enlace de los DOLs de prueba (D-034): el código desde 0x80003100, justo
 * después de la memoria baja que usan el IPL y el sistema operativo, y la pila
 * hacia abajo desde 0x81700000 (deja libre el último MB de los 24).
 *
 * Dos segmentos: texto (ejecutable) y datos (con el BSS al final). Cada sección
 * termina alineada a 32 bytes, como las copia el IPL.
 */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x80003100;
    .text : {
        *(.init)
        *(.text .text.*)
        . = ALIGN(32);
    } :text

    .data : {
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.sdata .sdata.* .sdata2 .sdata2.*)
        *(.got .got2)
        . = ALIGN(32);
    } :data

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.* .sbss2 .sbss2.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(32);
        __bss_end = .;
    } :data

    _stack_top = 0x81700000;

    /DISCARD/ : {
        *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) *(.PPC.EMB.apuinfo)
    }
}
