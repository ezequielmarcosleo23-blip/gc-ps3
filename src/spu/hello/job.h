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
 * Trabajo de prueba compartido entre el PPU y el programa SPE "hello".
 * El SPE lo trae por DMA, calcula out = a * b + a y lo devuelve.
 * Los valores de prueba son exactos en float para que el resultado no dependa
 * del redondeo (la SPU redondea hacia cero; el PPU, al más cercano).
 */
#ifndef GCPS3_SPU_HELLO_JOB_H
#define GCPS3_SPU_HELLO_JOB_H

#include <stdint.h>

typedef struct hello_job {
    float    a[4];
    float    b[4];
    float    out[4];
    uint32_t done;
    uint32_t pad[3];
} __attribute__((aligned(128))) hello_job_t;

_Static_assert(sizeof(float) * 12 + 16 == 64, "hello_job_t: 64 bytes útiles");

#endif
