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

/* El EFB (ver efb.h): VideoBackends/Software/SWEfbInterface.cpp de Dolphin. */
#include "core/crc32.h"
#include "gx/efb.h"

static inline uint32_t at(uint32_t x, uint32_t y)
{
    return x + y * GX_EFB_WIDTH;
}

/* Convert6To8 */
static inline uint32_t c6to8(uint32_t v)
{
    return (v << 2) | (v >> 4);
}

static void set_alpha_color(uint32_t *p, uint32_t pf, uint32_t src)
{
    switch (pf) {
    case GX_PF_RGB8_Z24:
    case GX_PF_Z24:
    case GX_PF_RGB565_Z16:
        *p = src >> 8;
        return;
    case GX_PF_RGBA6_Z24:
        *p = ((src >> 2) & 0x00003Fu) | ((src >> 4) & 0x000FC0u) | ((src >> 6) & 0x03F000u) |
             ((src >> 8) & 0xFC0000u);
        return;
    default:
        return;                                     /* "Unsupported pixel format" */
    }
}

static void set_color_only(uint32_t *p, uint32_t pf, uint32_t src)
{
    switch (pf) {
    case GX_PF_RGB8_Z24:
    case GX_PF_Z24:
    case GX_PF_RGB565_Z16:
        *p = src >> 8;
        return;
    case GX_PF_RGBA6_Z24:
        *p = (*p & 0x00003Fu) | ((src >> 4) & 0x000FC0u) | ((src >> 6) & 0x03F000u) |
             ((src >> 8) & 0xFC0000u);
        return;
    default:
        return;
    }
}

static void set_alpha_only(uint32_t *p, uint32_t pf, uint32_t a)
{
    if (pf == GX_PF_RGBA6_Z24)
        *p = (*p & 0xFFFFC0u) | ((a >> 2) & 0x3Fu);
}

void gx_efb_set_color(gx_efb_t *e, const gx_efb_write_t *w, uint32_t x, uint32_t y, uint32_t rgba)
{
    uint32_t *p = &e->color[at(x, y)];
    if (w->color_update) {
        if (w->alpha_update)
            set_alpha_color(p, w->pf, rgba);
        else
            set_color_only(p, w->pf, rgba);
    } else if (w->alpha_update) {
        set_alpha_only(p, w->pf, rgba & 0xFFu);
    }
}

void gx_efb_set_depth(gx_efb_t *e, const gx_efb_write_t *w, uint32_t x, uint32_t y, uint32_t z)
{
    if (w->z_update && w->pf <= GX_PF_Z24)
        e->depth[at(x, y)] = z & 0xFFFFFFu;
}

uint32_t gx_efb_get_color(const gx_efb_t *e, uint32_t pf, uint32_t x, uint32_t y)
{
    const uint32_t s = e->color[at(x, y)];
    switch (pf) {
    case GX_PF_RGB8_Z24:
    case GX_PF_Z24:
    case GX_PF_RGB565_Z16:
        return 0xFFu | s << 8;
    case GX_PF_RGBA6_Z24:
        return c6to8(s & 0x3Fu) | c6to8((s >> 6) & 0x3Fu) << 8 | c6to8((s >> 12) & 0x3Fu) << 16 |
               c6to8((s >> 18) & 0x3Fu) << 24;
    default:
        return 0;
    }
}

uint32_t gx_efb_get_depth(const gx_efb_t *e, uint32_t pf, uint32_t x, uint32_t y)
{
    return pf <= GX_PF_Z24 ? e->depth[at(x, y)] : 0;
}

void gx_efb_crc(const gx_efb_t *e, int x0, int y0, int x1, int y1, uint32_t *color, uint32_t *depth)
{
    for (int d = 0; d < 2; d++) {
        const uint32_t *plane = d ? e->depth : e->color;
        uint32_t crc = 0;
        for (int y = y0; y < y1; y++) {
            uint8_t row[GX_EFB_WIDTH * 3];
            uint32_t n = 0;
            for (int x = x0; x < x1; x++) {
                const uint32_t v = plane[at((uint32_t)x, (uint32_t)y)];
                row[n++] = (uint8_t)v;
                row[n++] = (uint8_t)(v >> 8);
                row[n++] = (uint8_t)(v >> 16);
            }
            crc = gc_crc32(crc, row, n);
        }
        if (d)
            *depth = crc;
        else
            *color = crc;
    }
}
