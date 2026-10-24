/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2014 Dolphin Emulator Project
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
 * Decodificadores de texturas (ver texdec.h): TexDecoder_DecodeTexel y
 * TexDecoder_DecodeTexelRGBA8FromTmem de Dolphin. Los datos de 16 bits son
 * big-endian (se leen de a bytes); IA8 guarda el alfa en el primer byte y la
 * intensidad en el segundo, en la textura y en la paleta.
 *
 * El texel y la textura entera usan la misma función por bloque: cambia solo
 * cómo se llega al bloque.
 */
#include <string.h>

#include "gx/texdec.h"

/* SafeSpanRead<u8>: afuera, 0 */
static inline uint32_t rd8(gx_span_t s, uint32_t off)
{
    return off < s.n ? s.p[off] : 0;
}

/* Un u16 big-endian (SafeSpanRead<u16> y swap16); un byte afuera queda en 0 */
static inline uint32_t rd16be(gx_span_t s, uint32_t off)
{
    return rd8(s, off) << 8 | rd8(s, off + 1);
}

static inline uint32_t c3to8(uint32_t v) { return ((v << 5) | (v << 2) | (v >> 1)) & 0xFFu; }
static inline uint32_t c4to8(uint32_t v) { return ((v << 4) | v) & 0xFFu; }
static inline uint32_t c5to8(uint32_t v) { return ((v << 3) | (v >> 2)) & 0xFFu; }
static inline uint32_t c6to8(uint32_t v) { return ((v << 2) | (v >> 4)) & 0xFFu; }

static inline void put(uint8_t out[4], uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    out[0] = (uint8_t)r;
    out[1] = (uint8_t)g;
    out[2] = (uint8_t)b;
    out[3] = (uint8_t)a;
}

/* DecodePixel_IA8: alfa en el byte de arriba de la palabra big-endian */
static void ia8(uint8_t out[4], uint32_t be)
{
    const uint32_t i = be & 0xFFu, a = be >> 8;
    put(out, i, i, i, a);
}

static void rgb565(uint8_t out[4], uint32_t v)
{
    put(out, c5to8((v >> 11) & 0x1Fu), c6to8((v >> 5) & 0x3Fu), c5to8(v & 0x1Fu), 0xFF);
}

static void rgb5a3(uint8_t out[4], uint32_t v)
{
    if (v & 0x8000u)
        put(out, c5to8((v >> 10) & 0x1Fu), c5to8((v >> 5) & 0x1Fu), c5to8(v & 0x1Fu), 0xFF);
    else
        put(out, c4to8((v >> 8) & 0xFu), c4to8((v >> 4) & 0xFu), c4to8(v & 0xFu), c3to8((v >> 12) & 0x7u));
}

/* DecodePixel_Paletted con la entrada index de la paleta */
static void paletted(uint8_t out[4], gx_span_t tlut, uint32_t index, uint32_t tlutfmt)
{
    const uint32_t v = rd16be(tlut, 2 * index);
    switch (tlutfmt) {
    case GX_TL_IA8: ia8(out, v); return;
    case GX_TL_RGB565: rgb565(out, v); return;
    case GX_TL_RGB5A3: rgb5a3(out, v); return;
    default: put(out, 0, 0, 0, 0); return;
    }
}

int gx_tex_valid_format(uint32_t fmt)
{
    return fmt <= GX_TF_RGBA8 || (fmt >= GX_TF_C4 && fmt <= GX_TF_C14X2) || fmt == GX_TF_CMPR;
}

uint32_t gx_tex_block_width(uint32_t fmt)
{
    switch (fmt) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: case GX_TF_CMPR:
        return 8;
    case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_C14X2: case GX_TF_RGBA8:
        return 4;
    default:
        return 0;
    }
}

uint32_t gx_tex_block_height(uint32_t fmt)
{
    switch (fmt) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR:
        return 8;
    case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3:
    case GX_TF_C14X2: case GX_TF_RGBA8:
        return 4;
    default:
        return 0;
    }
}

uint32_t gx_tex_nibbles(uint32_t fmt)
{
    switch (fmt) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: return 1;
    case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: return 2;
    case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_C14X2: return 4;
    case GX_TF_RGBA8: return 8;
    default: return 0;
    }
}

/* Bytes de un bloque en la memoria */
static uint32_t block_bytes(uint32_t fmt)
{
    return fmt == GX_TF_RGBA8 ? 64 : 32;
}

/* CMPR: 4 sub-bloques de 4 × 4 con dos colores 565 y 2 bits por texel; el
 * color 3 con c1 <= c2 es el promedio transparente (no negro, como DXT1) */
static void cmpr(uint8_t out[4], gx_span_t src, uint32_t off, uint32_t ss, uint32_t tt)
{
    const uint32_t c1 = rd16be(src, off), c2 = rd16be(src, off + 2);
    const uint32_t b1 = c5to8(c1 & 0x1Fu), b2 = c5to8(c2 & 0x1Fu);
    const uint32_t g1 = c6to8((c1 >> 5) & 0x3Fu), g2 = c6to8((c2 >> 5) & 0x3Fu);
    const uint32_t r1 = c5to8((c1 >> 11) & 0x1Fu), r2 = c5to8((c2 >> 11) & 0x1Fu);
    uint32_t sel = (rd8(src, off + 4 + tt) >> (6 - (ss << 1))) & 3u;
    sel |= c1 > c2 ? 0 : 4;
#define BLEND(v1, v2) (((v1) * 3 + (v2) * 5) >> 3)
    switch (sel) {
    case 0: case 4: put(out, r1, g1, b1, 255); return;
    case 1: case 5: put(out, r2, g2, b2, 255); return;
    case 2: put(out, BLEND(r2, r1), BLEND(g2, g1), BLEND(b2, b1), 255); return;
    case 3: put(out, BLEND(r1, r2), BLEND(g1, g2), BLEND(b1, b2), 255); return;
    case 6: put(out, (r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 255); return;
    default: put(out, (r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 0); return;
    }
#undef BLEND
}

/* El texel (bs, bt) del bloque que empieza en el byte base */
static void texel_in_block(uint8_t out[4], gx_span_t src, uint32_t base, uint32_t bs, uint32_t bt,
                           uint32_t fmt, gx_span_t tlut, uint32_t tlutfmt)
{
    switch (fmt) {
    case GX_TF_C4:
    case GX_TF_I4: {
        const uint32_t off = (bt << 3) + bs;
        const uint32_t v = (rd8(src, base + (off >> 1)) >> ((off & 1u) ? 0 : 4)) & 0xFu;
        if (fmt == GX_TF_C4) {
            paletted(out, tlut, v, tlutfmt);
        } else {
            const uint32_t i = c4to8(v);
            put(out, i, i, i, i);
        }
        return;
    }
    case GX_TF_I8:
    case GX_TF_C8:
    case GX_TF_IA4: {
        const uint32_t v = rd8(src, base + (bt << 3) + bs);
        if (fmt == GX_TF_I8)
            put(out, v, v, v, v);
        else if (fmt == GX_TF_C8)
            paletted(out, tlut, v, tlutfmt);
        else
            put(out, c4to8(v & 0xFu), c4to8(v & 0xFu), c4to8(v & 0xFu), c4to8(v >> 4));
        return;
    }
    case GX_TF_IA8:
    case GX_TF_C14X2:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3: {
        const uint32_t v = rd16be(src, base + (((bt << 2) + bs) << 1));
        if (fmt == GX_TF_IA8)
            ia8(out, v);
        else if (fmt == GX_TF_C14X2)
            paletted(out, tlut, v & 0x3FFFu, tlutfmt);
        else if (fmt == GX_TF_RGB565)
            rgb565(out, v);
        else
            rgb5a3(out, v);
        return;
    }
    case GX_TF_RGBA8: {
        const uint32_t off = base + (((bt << 2) + bs) << 1);
        put(out, rd8(src, off + 1), rd8(src, off + 32), rd8(src, off + 33), rd8(src, off));
        return;
    }
    case GX_TF_CMPR:
        cmpr(out, src, base + ((((bt >> 2) << 1) + (bs >> 2)) << 3), bs & 3u, bt & 3u);
        return;
    default:
        return;                                 /* formato que no existe: no se escribe */
    }
}

void gx_tex_decode_texel(uint8_t out[4], gx_span_t src, int32_t s, int32_t t, int32_t width_m1,
                         uint32_t fmt, gx_span_t tlut, uint32_t tlutfmt)
{
    const uint32_t bw = gx_tex_block_width(fmt), bh = gx_tex_block_height(fmt);
    if (!bw)
        return;
    const uint32_t lw = bw == 8 ? 3 : 2, lh = bh == 8 ? 3 : 2;
    /* Los u16 de Dolphin */
    const uint32_t sblk = (uint16_t)(s >> lw), tblk = (uint16_t)(t >> lh);
    const uint32_t wblks = (uint16_t)((width_m1 >> lw) + 1);
    const uint32_t base = (tblk * wblks + sblk) * block_bytes(fmt);
    texel_in_block(out, src, base, (uint32_t)s & (bw - 1), (uint32_t)t & (bh - 1), fmt, tlut, tlutfmt);
}

void gx_tex_decode_texel_rgba8_tmem(uint8_t out[4], gx_span_t ar, gx_span_t gb, int32_t s, int32_t t,
                                    int32_t width_m1)
{
    const uint32_t sblk = (uint16_t)(s >> 2), tblk = (uint16_t)(t >> 2);
    const uint32_t wblks = (uint16_t)((width_m1 >> 2) + 1);
    const uint32_t off = (((tblk * wblks + sblk) << 4) + ((((uint32_t)t & 3u) << 2) + ((uint32_t)s & 3u))) << 1;
    put(out, rd8(ar, off + 1), rd8(gb, off), rd8(gb, off + 1), rd8(ar, off));
}

void gx_tex_decode(uint8_t *dst, gx_span_t src, uint32_t width, uint32_t height, uint32_t fmt,
                   gx_span_t tlut, uint32_t tlutfmt)
{
    const uint32_t bw = gx_tex_block_width(fmt), bh = gx_tex_block_height(fmt);
    if (!bw || !width || !height) {
        memset(dst, 0, (size_t)width * height * 4);
        return;
    }
    const uint32_t wblks = (width + bw - 1) / bw, hblks = (height + bh - 1) / bh;
    const uint32_t bytes = block_bytes(fmt);
    for (uint32_t by = 0; by < hblks; by++)
        for (uint32_t bx = 0; bx < wblks; bx++) {
            const uint32_t base = (by * wblks + bx) * bytes;
            for (uint32_t ty = 0; ty < bh; ty++) {
                const uint32_t y = by * bh + ty;
                if (y >= height)
                    break;
                for (uint32_t tx = 0; tx < bw; tx++) {
                    const uint32_t x = bx * bw + tx;
                    if (x >= width)
                        break;
                    texel_in_block(dst + 4 * ((size_t)y * width + x), src, base, tx, ty, fmt, tlut, tlutfmt);
                }
            }
        }
}
