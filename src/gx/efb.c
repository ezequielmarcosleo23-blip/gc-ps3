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
    uint32_t *p = &GX_EFB_COLOR(e)[at(x, y)];
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
        GX_EFB_DEPTH(e)[at(x, y)] = z & 0xFFFFFFu;
}

uint32_t gx_efb_get_color(const gx_efb_t *e, uint32_t pf, uint32_t x, uint32_t y)
{
    const uint32_t s = GX_EFB_COLOR(e)[at(x, y)];
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
    return pf <= GX_PF_Z24 ? GX_EFB_DEPTH(e)[at(x, y)] : 0;
}

/* ---- Z y mezcla (SWEfbInterface.cpp) ------------------------------------------------ */

static inline uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

int gx_efb_zcompare(gx_efb_t *e, const uint32_t *bp, uint32_t x, uint32_t y, uint32_t z)
{
    const uint32_t pf = bits(bp[0x43], 0, 3), zmode = bp[0x40];
    const uint32_t depth = gx_efb_get_depth(e, pf, x, y);
    int pass;
    switch (bits(zmode, 1, 3)) {
    case 0: pass = 0; break;
    case 1: pass = z < depth; break;
    case 2: pass = z == depth; break;
    case 3: pass = z <= depth; break;
    case 4: pass = z > depth; break;
    case 5: pass = z != depth; break;
    case 6: pass = z >= depth; break;
    default: pass = 1; break;
    }
    if (pass && bits(zmode, 4, 1) && pf <= GX_PF_Z24)
        GX_EFB_DEPTH(e)[at(x, y)] = z & 0xFFFFFFu;
    return pass;
}

/* Los bytes de un 0xRRGGBBAA en el orden de Dolphin (u8[4] en x86): a, b, g, r */
static inline uint32_t byte(uint32_t v, int i)
{
    return (v >> (8 * i)) & 0xFFu;
}

static inline uint32_t splat(uint32_t a)
{
    return a << 24 | a << 16 | a << 8 | a;
}

/* GetSourceFactor y GetDestinationFactor: los dos usan la misma tabla salvo
 * los modos 2 y 3 (el color del otro lado) */
static uint32_t blend_factor(uint32_t mode, uint32_t other, uint32_t src, uint32_t dst)
{
    switch (mode) {
    case 0: return 0;
    case 1: return 0xFFFFFFFFu;
    case 2: return other;
    case 3: return 0xFFFFFFFFu - other;
    case 4: return splat(byte(src, 0));
    case 5: return splat(0xFFu - byte(src, 0));
    case 6: return splat(byte(dst, 0));
    default: return splat(0xFFu - byte(dst, 0));
    }
}

static uint32_t blend_color(const uint32_t *bp, uint32_t src, uint32_t dst)
{
    const uint32_t bm = bp[0x41];
    uint32_t sf = blend_factor(bits(bm, 8, 3), dst, src, dst);
    uint32_t df = blend_factor(bits(bm, 5, 3), src, src, dst);
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        /* El bit de arriba de cada factor se suma: de 0 a 256 */
        uint32_t s = sf & 0xFFu, d = df & 0xFFu;
        s += s >> 7;
        d += d >> 7;
        const uint32_t c = (byte(src, i) * s + byte(dst, i) * d) >> 8;
        out |= (c > 255 ? 255 : c) << (8 * i);
        sf >>= 8;
        df >>= 8;
    }
    return out;
}

static uint32_t logic_op(uint32_t op, uint32_t s, uint32_t d)
{
    switch (op) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return s & ~d;
    case 3: return s;
    case 4: return ~s & d;
    case 5: return d;
    case 6: return s ^ d;
    case 7: return s | d;
    case 8: return ~(s | d);
    case 9: return ~(s ^ d);
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~(s & d);
    default: return 0xFFFFFFFFu;
    }
}

void gx_efb_blend(gx_efb_t *e, const uint32_t *bp, uint32_t x, uint32_t y, uint32_t color)
{
    const uint32_t bm = bp[0x41], pf = bits(bp[0x43], 0, 3);
    uint32_t dst = gx_efb_get_color(e, pf, x, y);

    if (bits(bm, 0, 1)) {
        if (bits(bm, 11, 1)) {                      /* resta, también el alfa */
            uint32_t out = 0;
            for (int i = 0; i < 4; i++) {
                const int32_t c = (int32_t)byte(dst, i) - (int32_t)byte(color, i);
                out |= (uint32_t)(c < 0 ? 0 : c) << (8 * i);
            }
            dst = out;
        } else {
            dst = blend_color(bp, color, dst);
        }
    } else if (bits(bm, 1, 1)) {
        dst = logic_op(bits(bm, 12, 4), color, dst);
    } else {
        dst = color;
    }

    const uint32_t da = bp[0x42];
    if (bits(da, 8, 1))
        dst = (dst & 0xFFFFFF00u) | bits(da, 0, 8);

    gx_efb_write_t w = { pf, (int)bits(bm, 3, 1), (int)bits(bm, 4, 1), 0 };
    if (w.color_update && bits(bm, 2, 1) && pf == GX_PF_RGBA6_Z24) {
        /* Dither de 6 bits con la matriz de Bayer de 2 × 2, solo el color */
        static const uint32_t dither[2][2] = { { 0, 2 }, { 3, 1 } };
        const uint32_t d = dither[y & 1u][x & 1u];
        for (int i = 1; i <= 3; i++) {
            const uint32_t c = byte(dst, i);
            const uint32_t nc = ((c - (c >> 6)) + d) & 0xFCu;
            dst = (dst & ~(0xFFu << (8 * i))) | nc << (8 * i);
        }
    }
    gx_efb_set_color(e, &w, x, y, dst);
}

void gx_efb_crc(const gx_efb_t *e, int x0, int y0, int x1, int y1, uint32_t *color, uint32_t *depth)
{
    for (int d = 0; d < 2; d++) {
        const uint32_t *plane = d ? GX_EFB_DEPTH(e) : GX_EFB_COLOR(e);
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
