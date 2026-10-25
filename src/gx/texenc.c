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

/*
 * Copias del EFB a textura (texenc.h), como TextureEncoder.cpp de Dolphin
 * cf5cb7ef. Allá son seis funciones de lazos por formato; acá, un lazo por
 * bloques y un texel por formato, con los mismos valores:
 *
 *   - El texel (s, t) lee el píxel (s, t) desde el primero de la copia, o el
 *     cuadro de 2 × 2 en (2s, 2t) a media escala, de corrido en filas de 640:
 *     el relleno del último bloque sigue en la fila (o el plano) de abajo.
 *   - La cantidad de bloques sale de BP 0x4A sin recortar al EFB.
 *   - Desde RGBA6, cada componente de 6 bits pasa a 8 con (v << 2) | (v >> 4);
 *     a media escala se suman los 4 de 6 bits y se hace s + (s >> 6). Desde
 *     RGB8 (y RGB565, que Dolphin guarda igual) se promedian los bytes (>> 2).
 *   - Intensidad: (4096 + 66 r + 129 g + 25 b) >> 8, solo en R4, R8, RA4 y RA8.
 *   - Lo que Dolphin hace distinto según el camino (RGB5A3 con alfa >= 224 a
 *     media escala, el orden de RG8 y GB8 de la Z a media escala, RGBA8 de la Z
 *     a media escala con azul y rojo cambiados) se copia tal cual.
 */
#include "gx/texenc.h"

enum {
    CF_R4 = 0x0, CF_R8_1 = 0x1, CF_RA4 = 0x2, CF_RA8 = 0x3, CF_RGB565 = 0x4, CF_RGB5A3 = 0x5,
    CF_RGBA8 = 0x6, CF_A8 = 0x7, CF_R8 = 0x8, CF_G8 = 0x9, CF_B8 = 0xA, CF_RG8 = 0xB, CF_GB8 = 0xC,
};
enum { PF_RGB8 = 0, PF_RGBA6 = 1, PF_RGB565 = 2, PF_Z24 = 3 };
enum { K_RGB8, K_RGBA6, K_Z24 };

#define ROW 640u

static inline uint8_t c6to8(uint32_t v)
{
    return (uint8_t)((v << 2) | (v >> 4));
}

static inline uint8_t intensity(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint8_t)((uint16_t)(4096 + 66 * r + 129 * g + 25 * b) >> 8);
}

static inline uint32_t pix(gx_pixels_t src, uint32_t i)
{
    return i < src.n ? src.p[i] & 0xFFFFFFu : 0;
}

/* Bloques: ancho y alto en log2 y bytes; 0 si el formato no existe */
static int layout(uint32_t kind, uint32_t fmt, unsigned *lw, unsigned *lh, uint32_t *bpb)
{
    *bpb = 32;
    switch (fmt) {
    case CF_R4:
        *lw = 3;
        *lh = 3;
        return 1;
    case CF_R8_1: case CF_R8: case CF_G8: case CF_B8:
        *lw = 3;
        *lh = 2;
        return 1;
    case CF_RA4: case CF_A8:
        *lw = 3;
        *lh = 2;
        return kind != K_Z24;                   /* la Z no los tiene: PanicAlert */
    case CF_RG8: case CF_GB8:
        *lw = 2;
        *lh = 2;
        return 1;
    case CF_RGBA8:
        *lw = 2;
        *lh = 2;
        *bpb = 64;
        return 1;
    case CF_RA8: case CF_RGB565: case CF_RGB5A3:
        *lw = 2;
        *lh = 2;
        return kind != K_Z24;
    default:
        return 0;                               /* "Unknown texture copy format" */
    }
}

static int kind_of(uint32_t efb_fmt, uint32_t *kind)
{
    switch (efb_fmt) {
    case PF_RGB8: case PF_RGB565: *kind = K_RGB8; return 1;
    case PF_RGBA6: *kind = K_RGBA6; return 1;
    case PF_Z24: *kind = K_Z24; return 1;
    default: return 0;                          /* EncodeEfbCopy no escribe nada */
    }
}

uint32_t gx_texenc_extent(const gx_texenc_t *c)
{
    uint32_t kind, bpb;
    unsigned lw, lh;
    if (!kind_of(c->efb_fmt, &kind) || !layout(kind, c->copy_fmt, &lw, &lh, &bpb))
        return 0;
    const uint32_t nbx = ((c->wm1 >> c->half) >> lw) + 1, nby = ((c->hm1 >> c->half) >> lh) + 1;
    return (nby - 1) * c->stride + nbx * bpb;
}

/* ---- un texel: r, g, b, a de 8 bits como los arma Dolphin --------------------------- */

typedef struct texel {
    uint8_t r, g, b, a;         /* RGBA6: convertidos; RGB8 y Z: los bytes 2, 1, 0 */
} texel_t;

/* El píxel i, de una (texel s, t sin escala) o el cuadro de 2 × 2 */
static texel_t fetch(gx_pixels_t src, uint32_t kind, int half, uint32_t i)
{
    texel_t t;
    if (!half) {
        const uint32_t v = pix(src, i);
        if (kind == K_RGBA6) {
            t.r = c6to8((v >> 18) & 0x3Fu);
            t.g = c6to8((v >> 12) & 0x3Fu);
            t.b = c6to8((v >> 6) & 0x3Fu);
            t.a = c6to8(v & 0x3Fu);
        } else {
            t.r = (uint8_t)(v >> 16);
            t.g = (uint8_t)(v >> 8);
            t.b = (uint8_t)v;
            t.a = 0xFF;
        }
        return t;
    }
    const uint32_t v[4] = { pix(src, i), pix(src, i + 1), pix(src, i + ROW), pix(src, i + ROW + 1) };
    uint32_t r = 0, g = 0, b = 0, a = 0;
    for (int k = 0; k < 4; k++) {
        if (kind == K_RGBA6) {
            r += (v[k] >> 18) & 0x3Fu;
            g += (v[k] >> 12) & 0x3Fu;
            b += (v[k] >> 6) & 0x3Fu;
            a += v[k] & 0x3Fu;
        } else {
            r += (v[k] >> 16) & 0xFFu;
            g += (v[k] >> 8) & 0xFFu;
            b += v[k] & 0xFFu;
        }
    }
    if (kind == K_RGBA6) {
        t.r = (uint8_t)(r + (r >> 6));
        t.g = (uint8_t)(g + (g >> 6));
        t.b = (uint8_t)(b + (b >> 6));
        t.a = (uint8_t)(a + (a >> 6));
    } else {
        t.r = (uint8_t)(r >> 2);
        t.g = (uint8_t)(g >> 2);
        t.b = (uint8_t)(b >> 2);
        t.a = 0xFF;
    }
    return t;
}

static inline void be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* RGB565 y RGB5A3: desde los bits del píxel sin escala, o desde el texel */
static uint32_t rgb565(uint32_t kind, int half, uint32_t v, texel_t t)
{
    if (kind == K_RGBA6 && !half)
        return ((v >> 8) & 0xF800u) | ((v >> 7) & 0x07E0u) | ((v >> 7) & 0x001Fu);
    return (((uint32_t)t.r << 8) & 0xF800u) | (((uint32_t)t.g << 3) & 0x07E0u) | ((t.b >> 3) & 0x001Fu);
}

static uint32_t rgb5a3(uint32_t kind, int half, uint32_t v, texel_t t)
{
    if (kind == K_RGBA6 && !half) {
        const uint32_t alpha = (v << 9) & 0x7000u;
        if (alpha == 0x7000u)
            return 0x8000u | ((v >> 9) & 0x7C00u) | ((v >> 8) & 0x03E0u) | ((v >> 7) & 0x001Fu);
        return alpha | ((v >> 12) & 0x0F00u) | ((v >> 10) & 0x00F0u) | ((v >> 8) & 0x000Fu);
    }
    if (kind == K_RGBA6 && t.a < 224)
        return (((uint32_t)t.a << 7) & 0x7000u) | (((uint32_t)t.r << 4) & 0x0F00u) | (t.g & 0x00F0u) |
               ((t.b >> 4) & 0x000Fu);
    return 0x8000u | (((uint32_t)t.r << 7) & 0x7C00u) | (((uint32_t)t.g << 2) & 0x03E0u) |
           ((t.b >> 3) & 0x001Fu);
}

/* El valor de R4 y R8 (y lo de I en RA4 y RA8): R, o la intensidad */
static uint8_t red_or_i(uint32_t kind, int yuv, texel_t t)
{
    return (yuv && kind != K_Z24) ? intensity(t.r, t.g, t.b) : t.r;
}

void gx_texenc(uint8_t *dst, gx_pixels_t src, const gx_texenc_t *c)
{
    uint32_t kind, bpb;
    unsigned lw, lh;
    if (!kind_of(c->efb_fmt, &kind) || !layout(kind, c->copy_fmt, &lw, &lh, &bpb))
        return;
    const int half = c->half;
    const int yuv = c->intensity;
    const uint32_t k = half ? 2 : 1;
    const uint32_t bw = 1u << lw, bh = 1u << lh;
    const uint32_t nbx = ((c->wm1 >> half) >> lw) + 1, nby = ((c->hm1 >> half) >> lh) + 1;

    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++) {
            uint8_t *blk = dst + by * c->stride + bx * bpb;
            for (uint32_t t = 0; t < bh; t++)
                for (uint32_t s = 0; s < bw; s++) {
                    const uint32_t i = k * ((by * bh + t) * ROW + bx * bw + s);
                    const uint32_t v = pix(src, i);
                    const texel_t x = fetch(src, kind, half, i);
                    const uint32_t n = t * bw + s;  /* texel en el bloque */
                    switch (c->copy_fmt) {
                    case CF_R4: {
                        uint8_t q;
                        if (kind == K_RGBA6 && !half && !yuv)
                            q = (uint8_t)((s & 1u) ? (v >> 20) & 0x0Fu : (v >> 16) & 0xF0u);
                        else {
                            const uint8_t r = red_or_i(kind, yuv, x);
                            q = (uint8_t)((s & 1u) ? r >> 4 : r & 0xF0u);
                        }
                        uint8_t *p = &blk[n >> 1];
                        *p = (s & 1u) ? (uint8_t)(*p | q) : q;
                        break;
                    }
                    case CF_R8_1:
                    case CF_R8:
                        blk[n] = red_or_i(kind, yuv, x);
                        break;
                    case CF_RA4:
                        if (kind == K_RGBA6 && !half && !yuv)
                            blk[n] = (uint8_t)(((v << 2) & 0xF0u) | ((v >> 20) & 0x0Fu));
                        else if (kind == K_RGBA6)
                            blk[n] = (uint8_t)((x.a & 0xF0u) | (red_or_i(kind, yuv, x) >> 4));
                        else
                            blk[n] = (uint8_t)(0xF0u | (red_or_i(kind, yuv, x) >> 4));
                        break;
                    case CF_RA8:
                        blk[2 * n] = x.a;
                        blk[2 * n + 1] = red_or_i(kind, yuv, x);
                        break;
                    case CF_RGB565:
                        be16(&blk[2 * n], rgb565(kind, half, v, x));
                        break;
                    case CF_RGB5A3:
                        be16(&blk[2 * n], rgb5a3(kind, half, v, x));
                        break;
                    case CF_RGBA8:
                        blk[2 * n] = x.a;
                        if (kind == K_Z24 && half) {    /* Dolphin: b, g, r al revés */
                            blk[2 * n + 1] = x.b;
                            blk[32 + 2 * n] = x.g;
                            blk[33 + 2 * n] = x.r;
                        } else {
                            blk[2 * n + 1] = x.r;
                            blk[32 + 2 * n] = x.g;
                            blk[33 + 2 * n] = x.b;
                        }
                        break;
                    case CF_A8:
                        blk[n] = x.a;               /* desde RGB8: 0xFF */
                        break;
                    case CF_G8:
                        blk[n] = x.g;
                        break;
                    case CF_B8:
                        blk[n] = x.b;
                        break;
                    case CF_RG8:
                        if (kind == K_Z24 && half) {    /* Dolphin: el byte 1 y el 0 */
                            blk[2 * n] = x.g;
                            blk[2 * n + 1] = x.b;
                        } else {
                            blk[2 * n] = x.g;
                            blk[2 * n + 1] = x.r;
                        }
                        break;
                    case CF_GB8:
                        if (kind == K_Z24 && half) {    /* Dolphin: el byte 2 y el 1 */
                            blk[2 * n] = x.r;
                            blk[2 * n + 1] = x.g;
                        } else {
                            blk[2 * n] = x.b;
                            blk[2 * n + 1] = x.g;
                        }
                        break;
                    default:
                        break;
                    }
                }
        }
}
