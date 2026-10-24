/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2009 Dolphin Emulator Project
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

/* El rasterizador (ver raster.h): VideoBackends/Software/Rasterizer.cpp. */
#include <stdio.h>
#include <string.h>

#include "gx/gx.h"
#include "gx/raster.h"

enum { BP_GENMODE = 0x00 };

static uint32_t fbits(float f)
{
    if (f != f)
        return 0x7fc00000u;
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

void gc_gx_triangle_text(const gx_rvtx_t *const v[3], uint32_t ntex, char buf[GC_GX_TRIANGLE_TEXT])
{
    size_t at = (size_t)snprintf(buf, GC_GX_TRIANGLE_TEXT, "t%u", (unsigned)ntex);
    for (int k = 0; k < 3; k++) {
        at += (size_t)snprintf(buf + at, GC_GX_TRIANGLE_TEXT - at, " v %08x %08x %08x %08x",
                               (unsigned)fbits(v[k]->scr[0]), (unsigned)fbits(v[k]->scr[1]),
                               (unsigned)fbits(v[k]->scr[2]), (unsigned)fbits(v[k]->proj[3]));
        for (int c = 0; c < 2; c++)
            at += (size_t)snprintf(buf + at, GC_GX_TRIANGLE_TEXT - at, " %02x%02x%02x%02x",
                                   v[k]->col[c][0], v[k]->col[c][1], v[k]->col[c][2], v[k]->col[c][3]);
        for (uint32_t i = 0; i < ntex && i < 8; i++)
            at += (size_t)snprintf(buf + at, GC_GX_TRIANGLE_TEXT - at, " %08x %08x %08x",
                                   (unsigned)fbits(v[k]->tex[i][0]), (unsigned)fbits(v[k]->tex[i][1]),
                                   (unsigned)fbits(v[k]->tex[i][2]));
    }
}

void gx_raster_update_zslope(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1,
                             const gx_rvtx_t *v2, int32_t x_off, int32_t y_off)
{
    /* La pendiente de Z llega con el rasterizador (C3) */
    (void)gx;
    (void)v0;
    (void)v1;
    (void)v2;
    (void)x_off;
    (void)y_off;
}

void gx_raster_triangle(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2)
{
    if (gx->triangle_cb) {
        const gx_rvtx_t *const v[3] = { v0, v1, v2 };
        gx->triangle_cb(gx->triangle_ctx, v, gx->bp[BP_GENMODE] & 15u);
    }
    gx->triangles++;
}
