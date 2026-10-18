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
 * Programa SPE "xform" como trabajador persistente de spukit (lib/spukit):
 * cada trabajo XFORM_KIND transforma un tramo de vértices. spukit usa la
 * etiqueta DMA 31; el núcleo, las 0 a 3.
 */
#include <string.h>
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "spu/xform/kernel.h"

static int handler(void *ctx, const spukit_job *job, spukit_result *res)
{
    xform_params_t p __attribute__((aligned(16)));

    (void)ctx;
    (void)res;
    if (job->kind != XFORM_KIND || job->size != sizeof p)
        return -2;
    memcpy(&p, job->data, sizeof p);
    xform_run(p.m, p.src_ea, p.dst_ea, p.count);
    return 0;
}

int main(uint64_t ctrl_ea, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg1; (void)arg2; (void)arg3;
    spukit_run(ctrl_ea, handler, NULL);
    spu_thread_exit(0);
    return 0;
}
