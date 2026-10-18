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
 * Programa SPE "xform" como hilo SPU directo (sin spukit): un trabajo por
 * ejecución. arg0 = dirección efectiva del xform_raw_job_t.
 */
#include <sys/spu_thread.h>

#include "spu/xform/kernel.h"

#define JOB_TAG 4

static xform_raw_job_t job;

int main(uint64_t job_ea, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1; (void)arg2; (void)arg3;

    mfc_get(&job, job_ea, sizeof job, JOB_TAG, 0, 0);
    xform_wait(1u << JOB_TAG);

    spu_write_decrementer(0xFFFFFFFFu);
    const uint32_t t0 = spu_read_decrementer();
    xform_run(job.p.m, job.p.src_ea, job.p.dst_ea, job.p.count);
    job.ticks = t0 - spu_read_decrementer();
    job.done = 1;

    mfc_put(&job, job_ea, sizeof job, JOB_TAG, 0, 0);
    xform_wait(1u << JOB_TAG);
    spu_thread_exit(0);
    return 0;
}
