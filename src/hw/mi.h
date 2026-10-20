/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008 Dolphin Emulator Project
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
 * Memory Interface (0x0C004000): regiones protegidas, interrupciones de
 * acceso y contadores. Como MemoryInterface.cpp de Dolphin (commit cf5cb7ef):
 * los registros de 16 bits guardan lo que se escribe y nada más; la
 * protección de memoria no se emula (Dolphin tampoco).
 */
#ifndef GCPS3_HW_MI_H
#define GCPS3_HW_MI_H

#include <stdint.h>

#define GC_MI_REGS_SIZE 0x5C

struct gc_system;

typedef struct gc_mi {
    struct gc_system *sys;
    uint16_t reg[GC_MI_REGS_SIZE / 2];
} gc_mi_t;

void gc_mi_init(gc_mi_t *mi, struct gc_system *sys);
void gc_mi_reset(gc_mi_t *mi);

#endif
