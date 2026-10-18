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
 * Programa SPE mínimo: comprueba que el build compila, incrusta y lanza código SPU.
 * arg0 = dirección efectiva del hello_job_t en memoria principal.
 */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include "spu/hello/job.h"

#define DMA_TAG 1

static hello_job_t job;   /* alineado a 128 B por su tipo */

static void dma_wait(void)
{
    mfc_write_tag_mask(1 << DMA_TAG);
    spu_mfcstat(MFC_TAG_UPDATE_ALL);
}

int main(uint64_t job_ea, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1; (void)arg2; (void)arg3;

    mfc_get(&job, job_ea, sizeof job, DMA_TAG, 0, 0);
    dma_wait();

    vec_float4 a = *(vec_float4 *)job.a;
    vec_float4 b = *(vec_float4 *)job.b;
    *(vec_float4 *)job.out = spu_madd(a, b, a);
    job.done = 1;

    mfc_put(&job, job_ea, sizeof job, DMA_TAG, 0, 0);
    dma_wait();

    spu_thread_exit(0);
    return 0;
}
