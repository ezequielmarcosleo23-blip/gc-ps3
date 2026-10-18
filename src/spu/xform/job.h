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
 * Trabajo "xform", compartido entre el PPU y los programas SPE xform (hilo SPU
 * directo) y xform_kit (trabajador de spukit): transforma `count` vértices
 * (x, y, z, w en float, 16 bytes cada uno) por una matriz 4x4:
 *
 *     out = M * v      (M por filas: m[fila * 4 + columna])
 *
 * El SPU trae los vértices por DMA en trozos de XFORM_CHUNK con doble búfer y
 * devuelve el resultado por DMA. src y dst alineados a 128 bytes.
 */
#ifndef GCPS3_SPU_XFORM_JOB_H
#define GCPS3_SPU_XFORM_JOB_H

#include <stdint.h>

#define XFORM_CHUNK  1024u     /* vértices por DMA: 16 KB, el máximo de una transferencia */
#define XFORM_KIND   1u        /* kind del trabajo en spukit */

/* Parámetros: 88 bytes; caben en los 112 de un trabajo de spukit. La matriz va
 * primero para quedar alineada a 16 dentro del descriptor. */
typedef struct xform_params {
    float    m[16];
    uint64_t src_ea;
    uint64_t dst_ea;
    uint32_t count;
    uint32_t pad;
} xform_params_t;

/* Hilo SPU directo: el PPU pasa la dirección de esto en arg0; el SPU lo lee,
 * trabaja y lo devuelve con done = 1 y ticks del decrementador. */
typedef struct xform_raw_job {
    xform_params_t p;
    uint32_t       done;
    uint32_t       ticks;
    uint32_t       reserved[8];
} __attribute__((aligned(128))) xform_raw_job_t;

_Static_assert(sizeof(xform_params_t) == 88, "xform_params_t: 88 bytes");
_Static_assert(sizeof(xform_raw_job_t) == 128, "xform_raw_job_t: 128 bytes");

#endif
