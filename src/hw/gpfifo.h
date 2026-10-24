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
 * Write-gather pipe del Gekko (0x0C008000): lo que la CPU escribe ahí se junta
 * y cada 32 bytes se copia a la RAM en el puntero de escritura del FIFO del PI,
 * que vuelve a la base después del bloque de END. Con el FIFO enlazado al CP,
 * cada bloque suma 32 a la distancia del CP y despierta a la GPU
 * (gc_cp_gather_pipe_bursted).
 *
 * Como Source/Core/Core/HW/GPFifo.cpp de Dolphin (commit cf5cb7ef): la
 * dirección se compara con la página entera (0x0C008000-0x0C008FFF), la copia
 * es en el acto (en la misma instrucción que completa el bloque) y no hay bit
 * de vuelta en el puntero del PI.
 */
#ifndef GCPS3_HW_GPFIFO_H
#define GCPS3_HW_GPFIFO_H

#include <stdint.h>

#define GC_GPFIFO_BLOCK 32u                       /* GATHER_PIPE_SIZE */
#define GC_GPFIFO_BUF   (GC_GPFIFO_BLOCK * 16)    /* GATHER_PIPE_EXTRA_SIZE */
#define GC_GPFIFO_SPAN  0x1000u                   /* la página de 0x0C008000 */

struct gc_system;

typedef struct gc_gpfifo {
    struct gc_system *sys;
    uint8_t  buf[GC_GPFIFO_BUF];
    uint32_t count;                               /* bytes juntados, menos de 32 entre escrituras */
    uint64_t bytes;                               /* total escrito por la CPU */
    uint32_t bad_copies;                          /* bloques con el puntero fuera de la RAM */
} gc_gpfifo_t;

void gc_gpfifo_init(gc_gpfifo_t *g, struct gc_system *sys);

/* Vacía lo juntado (encendido y PI_FIFO_RESET). */
void gc_gpfifo_reset(gc_gpfifo_t *g);

/* Una escritura de la CPU de 1, 2 o 4 bytes (big-endian). */
void gc_gpfifo_write(gc_gpfifo_t *g, uint32_t value, int size);

#endif
