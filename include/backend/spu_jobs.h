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
 * Interfaz propia de trabajos para los SPE (BORRADOR).
 *
 * Modelo previsto: cola de descriptores de 128 B en memoria principal y el
 * mailbox solo como "timbre"; sin espera activa larga en el PPE. Puede
 * implementarse con spukit o con hilos SPU propios. Se fija al terminar la
 * prueba "spu" de la Fase 0.
 */
#ifndef GCPS3_BACKEND_SPU_JOBS_H
#define GCPS3_BACKEND_SPU_JOBS_H

#include <stdint.h>

#define SPU_JOB_DESC_SIZE 128

typedef struct spu_job_desc {
    uint32_t kind;          /* tipo de trabajo (vértices, texturas, DSP...) */
    uint32_t flags;
    uint64_t in_ea;         /* entrada en memoria principal */
    uint64_t out_ea;        /* salida en memoria principal */
    uint32_t in_size;
    uint32_t out_size;
    uint64_t done_ea;       /* el SPE escribe aquí al terminar */
    uint8_t  params[SPU_JOB_DESC_SIZE - 40];
} __attribute__((aligned(128))) spu_job_desc_t;

_Static_assert(sizeof(spu_job_desc_t) == SPU_JOB_DESC_SIZE, "descriptor de 128 B");

#endif
