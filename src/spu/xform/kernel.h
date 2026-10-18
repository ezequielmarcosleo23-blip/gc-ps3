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
 * Núcleo SPU de "xform" (solo para programas SPU; lo incluyen xform y
 * xform_kit). Doble búfer: mientras se calcula el trozo c llega el c + 1.
 *
 *   etiquetas DMA 0 y 1: entrada (búfer 0 y 1)
 *   etiquetas DMA 2 y 3: salida (búfer 0 y 1)
 *
 * Local store: 2 x 16 KB de entrada + 2 x 16 KB de salida.
 */
#ifndef GCPS3_SPU_XFORM_KERNEL_H
#define GCPS3_SPU_XFORM_KERNEL_H

#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#include "spu/xform/job.h"

static vec_float4 xf_in[2][XFORM_CHUNK] __attribute__((aligned(128)));
static vec_float4 xf_out[2][XFORM_CHUNK] __attribute__((aligned(128)));

static inline void xform_wait(uint32_t tag_mask)
{
    mfc_write_tag_mask(tag_mask);
    mfc_read_tag_status_all();
}

static inline uint32_t xform_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/* out = M * v para `count` vértices. Vuelve con todas las transferencias hechas. */
static inline void xform_run(const float *m, uint64_t src_ea, uint64_t dst_ea, uint32_t count)
{
    /* Columnas de M: out = c0 * x + c1 * y + c2 * z + c3 * w */
    const vec_float4 c0 = { m[0], m[4], m[8],  m[12] };
    const vec_float4 c1 = { m[1], m[5], m[9],  m[13] };
    const vec_float4 c2 = { m[2], m[6], m[10], m[14] };
    const vec_float4 c3 = { m[3], m[7], m[11], m[15] };
    const uint32_t chunks = (count + XFORM_CHUNK - 1) / XFORM_CHUNK;

    if (!chunks)
        return;
    mfc_get(xf_in[0], src_ea, xform_min(count, XFORM_CHUNK) * 16, 0, 0, 0);

    for (uint32_t c = 0; c < chunks; c++) {
        const uint32_t b = c & 1;
        const uint32_t first = c * XFORM_CHUNK;
        const uint32_t n = xform_min(XFORM_CHUNK, count - first);

        if (c + 1 < chunks) {                   /* pide el siguiente trozo */
            const uint32_t next = first + XFORM_CHUNK;
            mfc_get(xf_in[b ^ 1], src_ea + (uint64_t)next * 16,
                    xform_min(XFORM_CHUNK, count - next) * 16, b ^ 1, 0, 0);
        }
        xform_wait(1u << b);                    /* entrada c lista */
        xform_wait(1u << (2 + b));              /* salida de c - 2 ya enviada */

        const vec_float4 *in = xf_in[b];
        vec_float4 *out = xf_out[b];
        for (uint32_t i = 0; i < n; i++) {
            const vec_float4 v = in[i];
            vec_float4 r = spu_mul(c0, spu_splats(spu_extract(v, 0)));
            r = spu_madd(c1, spu_splats(spu_extract(v, 1)), r);
            r = spu_madd(c2, spu_splats(spu_extract(v, 2)), r);
            r = spu_madd(c3, spu_splats(spu_extract(v, 3)), r);
            out[i] = r;
        }
        mfc_put(out, dst_ea + (uint64_t)first * 16, n * 16, 2 + b, 0, 0);
    }
    xform_wait(0xFu);
}

#endif
