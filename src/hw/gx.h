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
 * Lo de GX que la CPU ve antes de que haya gráficos (Fase 1): los registros
 * del Command Processor (0x0C000000) y del Pixel Engine (0x0C001000), y la
 * entrada del FIFO de GX (0x0C008000, el write-gather pipe).
 *
 * Los registros guardan lo que se escribe, con las máscaras de Dolphin
 * (CommandProcessor.cpp y PixelEngine.cpp, commit cf5cb7ef) en las
 * direcciones del FIFO. CP informa siempre que está ocioso; el PE no levanta
 * tokens ni "finish". Lo que se escribe en el FIFO se cuenta y se descarta:
 * GX llega después de la Fase 1.
 */
#ifndef GCPS3_HW_GX_H
#define GCPS3_HW_GX_H

#include <stdint.h>

/* Registros del CP (de 16 bits) */
enum { CP_SR = 0x00, CP_CR = 0x02, CP_CLEAR = 0x04, CP_FIFO_FIRST = 0x20, CP_FIFO_LAST = 0x3E };
#define CP_SR_IDLE 0x000Cu        /* ReadIdle y CommandIdle */

/* Registros del PE (de 16 bits) */
enum { PE_CTRL = 0x0A, PE_TOKEN = 0x0E };

#define GC_CP_REGS_SIZE 0x80
#define GC_PE_REGS_SIZE 0x30

struct gc_system;

typedef struct gc_gx {
    struct gc_system *sys;
    uint16_t cp[GC_CP_REGS_SIZE / 2];
    uint16_t pe[GC_PE_REGS_SIZE / 2];
    uint64_t fifo_bytes;          /* bytes escritos en el FIFO de GX (descartados) */
} gc_gx_t;

void gc_gx_init(gc_gx_t *gx, struct gc_system *sys);
void gc_gx_reset(gc_gx_t *gx);

#endif
