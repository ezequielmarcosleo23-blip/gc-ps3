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

/* La parte de la mini-librería de GX para vértices, matrices y luces (Fase 2,
 * etapa B; ver gxlib.h). Va en otro archivo para que gxlib.o, y con él
 * gx_fifo.dol y su traza de referencia, no cambien. */
#include "gxlib.h"

void gx_u8(uint32_t v)
{
    WGPIPE8 = (uint8_t)v;
}

void gx_u16(uint32_t v)
{
    WGPIPE16 = (uint16_t)v;
}

void gx_u32(uint32_t v)
{
    WGPIPE32 = v;
}

uint32_t f32_bits(float f)
{
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

void gx_f32(float f)
{
    WGPIPE32 = f32_bits(f);
}

void gx_vat(uint32_t vat, uint32_t g0, uint32_t g1, uint32_t g2)
{
    gx_cp(CPR_VAT_A + vat, g0);
    gx_cp(CPR_VAT_B + vat, g1);
    gx_cp(CPR_VAT_C + vat, g2);
}

void gx_array(uint32_t array, const void *base, uint32_t stride)
{
    gx_cp(CPR_ARRAY_BASE + array, gx_phys(base));
    gx_cp(CPR_ARRAY_STRIDE + array, stride);
}

void gx_vtxdesc(uint32_t lo, uint32_t hi, int ntb)
{
    uint32_t colors = 0, texs = 0, normals = 0;
    if ((lo >> 13) & 3u)
        colors++;
    if ((lo >> 15) & 3u)
        colors++;
    if ((lo >> 11) & 3u)
        normals = ntb ? 2 : 1;
    for (int i = 0; i < 8; i++)
        if ((hi >> (2 * i)) & 3u)
            texs++;
    gx_cp(CPR_VCD_LO, lo);
    gx_cp(CPR_VCD_HI, hi);
    gx_xf1(XF_INVTXSPEC, colors | normals << 2 | texs << 4);
}

void gx_matindex(uint32_t a, uint32_t b)
{
    gx_cp(CPR_MATINDEX_A, a);
    gx_cp(CPR_MATINDEX_B, b);
    gx_xf1(XF_MATINDEX_A, a);
    gx_xf1(XF_MATINDEX_B, b);
}

void gx_xf_floats(uint32_t addr, uint32_t count, const float *values)
{
    WGPIPE8 = GX_LOAD_XF;
    WGPIPE32 = (count - 1) << 16 | addr;
    for (uint32_t k = 0; k < count; k++)
        gx_f32(values[k]);
}

void gx_pos_matrix(uint32_t idx, const float m[12])
{
    gx_xf_floats(XF_POS_MTX + idx * 4, 12, m);
}

void gx_normal_matrix(uint32_t idx, const float m[9])
{
    gx_xf_floats(XF_NORMAL_MTX + idx * 3, 9, m);
}

void gx_post_matrix(uint32_t idx, const float m[12])
{
    gx_xf_floats(XF_POST_MTX + idx * 4, 12, m);
}

void gx_light(uint32_t i, uint32_t color, const float cosatt[3], const float distatt[3],
              const float pos[3], const float dir[3])
{
    WGPIPE8 = GX_LOAD_XF;
    WGPIPE32 = 15u << 16 | (XF_LIGHTS + 16 * i);
    for (int k = 0; k < 3; k++)
        WGPIPE32 = 0;
    WGPIPE32 = color;
    for (int k = 0; k < 3; k++)
        gx_f32(cosatt[k]);
    for (int k = 0; k < 3; k++)
        gx_f32(distatt[k]);
    for (int k = 0; k < 3; k++)
        gx_f32(pos[k]);
    for (int k = 0; k < 3; k++)
        gx_f32(dir[k]);
}

void gx_projection(const float p[6], uint32_t ortho)
{
    WGPIPE8 = GX_LOAD_XF;
    WGPIPE32 = 6u << 16 | XF_PROJECTION;
    for (int k = 0; k < 6; k++)
        gx_f32(p[k]);
    WGPIPE32 = ortho;
}

void dl_u16(gx_dl_t *dl, uint32_t v)
{
    dl_u8(dl, v >> 8);
    dl_u8(dl, v);
}
