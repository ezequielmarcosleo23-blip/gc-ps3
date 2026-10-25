/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008 Dolphin Emulator Project
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
 * Copias del EFB (BP 0x52, Fase 2, etapa C): VideoCommon/BPStructs.cpp
 * (BPMEM_TRIGGER_EFB_COPY), TextureCacheBase::ResolveFramebufferCopyData,
 * BPFunctions::ClearScreen y, del renderizador por software, EfbCopy::ClearEfb.
 *
 *   - El rectángulo es [x, x + ancho] × [y, y + alto] de BP 0x49 y 0x4A. Si se
 *     pasa del EFB se recorta; si empieza afuera, no se copia ni se borra.
 *   - Con paso de destino (BP 0x4D) distinto de cero se copia a la RAM en
 *     BP 0x4B << 5: a textura o al XFB (con la escala vertical de BP 0x4E).
 *   - Con el bit de borrar, si se escribe algo (color, alfa en RGBA6 o Z), el
 *     video por software borra todo el rectángulo pedido con el color y la Z de
 *     BP 0x4F-0x51, a través de las máscaras de escritura.
 *
 * Lo que va a la RAM pasa por el búfer intermedio de Dolphin (TextureCacheBase
 * con el video por software, que copia en el momento): el codificador escribe
 * las filas de bloques cada "paso" (BP 0x4D) y después se copian a la RAM
 * min(bytes por fila, paso) bytes de cada fila. Si el paso no alcanza para una
 * fila, la imagen sale mezclada, como en Dolphin; si en la copia al XFB se leen
 * más filas de las que se escribieron, sale lo que dejó una copia anterior.
 *
 * Con copy_cb, una línea por copia como la del parche de Dolphin
 * (GCPS3_COPIAS): formato de píxel, BP 0x52, rectángulo, destino, CRC del EFB
 * (después de copiar y antes de borrar) y de los bytes que quedaron en la RAM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/crc32.h"
#include "core/log.h"
#include "core/system.h"
#include "gx/efb.h"
#include "gx/gx.h"
#include "gx/raster.h"
#include "gx/texenc.h"
#include "gx/x86.h"

#define LOG_MAX 16

enum {
    BP_ZMODE = 0x40, BP_BLENDMODE = 0x41, BP_ZCONTROL = 0x43, BP_COPY_SRC_XY = 0x49,
    BP_COPY_SRC_WH = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D, BP_COPY_YSCALE = 0x4E,
    BP_CLEAR_AR = 0x4F, BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51, BP_COPY = 0x52, BP_COPY_FILTER0 = 0x53,
    BP_COPY_FILTER1 = 0x54,
};

/* Formatos de textura (TextureDecoder.h) */
enum {
    TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6,
    TF_XFB = 0xF,
};
#define EFB_COPY_XFB 0x10u                         /* EFBCopyFormat::XFB */

static uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

gx_efb_write_t gc_gx_efb_write_mask(const gc_gx_t *gx)
{
    gx_efb_write_t w;
    w.pf = bits(gx->bp[BP_ZCONTROL], 0, 3);
    w.color_update = (int)bits(gx->bp[BP_BLENDMODE], 3, 1);
    w.alpha_update = (int)bits(gx->bp[BP_BLENDMODE], 4, 1);
    w.z_update = (int)bits(gx->bp[BP_ZMODE], 4, 1);
    return w;
}

/* TexDecoder_GetEFBCopyBaseFormat */
static uint32_t base_format(uint32_t efb_fmt)
{
    switch (efb_fmt) {
    case 0x0: return TF_I4;
    case 0x1: case 0x7: case 0x8: case 0x9: case 0xA: return TF_I8;
    case 0x2: return TF_IA4;
    case 0x3: case 0xB: case 0xC: return TF_IA8;
    case 0x4: return TF_RGB565;
    case 0x5: return TF_RGB5A3;
    case 0x6: return TF_RGBA8;
    case EFB_COPY_XFB: return TF_XFB;
    default: return efb_fmt;                        /* Dolphin: PanicAlert */
    }
}

/* TexDecoder_GetBlockWidthInTexels / Height (los inválidos, como Dolphin: 8 y 4) */
static uint32_t block_w(uint32_t tf)
{
    switch (tf) {
    case TF_IA8: case TF_RGB565: case TF_RGB5A3: case TF_RGBA8: case 0xA: return 4;
    case TF_XFB: return 16;
    default: return 8;
    }
}

static uint32_t block_h(uint32_t tf)
{
    switch (tf) {
    case TF_I4: case 0x8: case 0xE: return 8;
    case TF_XFB: return 1;
    default: return 4;
    }
}

/* Lo que ocupa en la RAM (ResolveFramebufferCopyData) */
static void copy_size(uint32_t efb_fmt, int half, uint32_t w, uint32_t h, uint32_t *bytes_per_row,
                      uint32_t *rows)
{
    if (half) {
        w /= 2;
        h /= 2;
    }
    const uint32_t tf = base_format(efb_fmt), bw = block_w(tf), bh = block_h(tf);
    *rows = (h + bh - 1) / bh;
    *bytes_per_row = (w + bw - 1) / bw * (tf == TF_RGBA8 ? 64u : 32u);
}

static void log_line(gc_gx_t *gx, const char *text)
{
    if (gx->copy_cb)
        gx->copy_cb(gx->copy_ctx, text);
}

/* EfbCopy::ClearEfb, a través de BPFunctions::ClearScreen */
static void clear(gc_gx_t *gx)
{
    gx_efb_write_t w = gc_gx_efb_write_mask(gx);
    int alpha = w.alpha_update;
    if (w.pf == GX_PF_RGB8_Z24 || w.pf == GX_PF_RGB565_Z16 || w.pf == GX_PF_Z24)
        alpha = 0;
    if (!w.color_update && !alpha && !w.z_update)
        return;

    const uint32_t ar = gx->bp[BP_CLEAR_AR], gb = gx->bp[BP_CLEAR_GB];
    const uint32_t color = (ar & 0xFFu) << 24 | (gb & 0xFFFFu) << 8 | (ar & 0xFF00u) >> 8;
    const uint32_t xy = gx->bp[BP_COPY_SRC_XY], wh = gx->bp[BP_COPY_SRC_WH];
    const uint32_t left = bits(xy, 0, 10), top = bits(xy, 10, 10);
    uint32_t right = left + bits(wh, 0, 10), bottom = top + bits(wh, 10, 10);
    if (right > GX_EFB_WIDTH - 1)
        right = GX_EFB_WIDTH - 1;
    if (bottom > GX_EFB_HEIGHT - 1)
        bottom = GX_EFB_HEIGHT - 1;
    for (uint32_t y = top; y <= bottom; y++)
        for (uint32_t x = left; x <= right; x++) {
            gx_efb_set_color(gx->efb, &w, x, y, color);
            gx_efb_set_depth(gx->efb, &w, x, y, gx->bp[BP_CLEAR_Z]);
        }
}

/* ---- a la RAM ------------------------------------------------------------------------ */

/* El búfer intermedio con al menos n bytes (lo nuevo en 0, como el de Dolphin
 * al crearse); más que el de Dolphin no: ahí Dolphin escribe fuera */
static uint8_t *staging(gc_gx_t *gx, uint32_t n)
{
    if (n > GX_STAGING_MAX)
        return NULL;
    if (n > gx->staging_size) {
        uint8_t *p = realloc(gx->staging, n);
        if (!p)
            return NULL;
        memset(p + gx->staging_size, 0, n - gx->staging_size);
        gx->staging = p;
        gx->staging_size = n;
    }
    return gx->staging;
}

/* AbstractStagingTexture::ReadTexels: min(bytes por fila, paso) de cada fila */
static void staging_to_ram(gc_gx_t *gx, uint8_t *ram, uint32_t stride, uint32_t bytes_per_row, uint32_t rows)
{
    const uint32_t n = bytes_per_row < stride ? bytes_per_row : stride;
    for (uint32_t k = 0; k < rows; k++)
        memcpy(ram + k * stride, gx->staging + k * stride, n);
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

static void texture_copy(gc_gx_t *gx, uint32_t trig, int l, int t, uint8_t *ram, uint32_t stride,
                         uint32_t bytes_per_row, uint32_t rows)
{
    const uint32_t pf = bits(gx->bp[BP_ZCONTROL], 0, 3);
    const uint32_t tp = bits(trig, 3, 4), wh = gx->bp[BP_COPY_SRC_WH];
    gx_texenc_t c;
    c.efb_fmt = pf;
    c.copy_fmt = tp / 2 + (tp & 1u) * 8;
    c.intensity = bits(trig, 15, 1) && bits(trig, 16, 1);
    c.half = (int)bits(trig, 9, 1);
    c.wm1 = bits(wh, 0, 10);
    c.hm1 = bits(wh, 10, 10);
    c.stride = stride;
    const uint32_t first = (pf == GX_PF_Z24 ? GX_EFB_PIXELS : 0) + (uint32_t)t * GX_EFB_WIDTH + (uint32_t)l;
    const gx_pixels_t src = { gx->efb->px + first, 2 * GX_EFB_PIXELS - first };
    const uint32_t read = rows ? (rows - 1) * stride + (bytes_per_row < stride ? bytes_per_row : stride) : 0;
    if (!staging(gx, max_u32(gx_texenc_extent(&c), read))) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: copia del EFB demasiado grande (paso %u, %u filas)", (unsigned)stride, (unsigned)rows);
        return;
    }
    gx_texenc(gx->staging, src, &c);
    staging_to_ram(gx, ram, stride, bytes_per_row, rows);
}

static void xfb_copy(gc_gx_t *gx, uint32_t trig, int l, int t, int r, int b, float scale, uint8_t *ram,
                     uint32_t stride, uint32_t bytes_per_row, uint32_t rows)
{
    const uint32_t f0 = gx->bp[BP_COPY_FILTER0], f1 = gx->bp[BP_COPY_FILTER1];
    gx_xfbenc_t c;
    c.pf = bits(gx->bp[BP_ZCONTROL], 0, 3);
    c.left = (uint32_t)l;
    c.top = (uint32_t)t;
    c.right = (uint32_t)r;
    c.bottom = (uint32_t)b;
    c.clamp_top = (int)bits(trig, 0, 1);
    c.clamp_bottom = (int)bits(trig, 1, 1);
    for (int k = 0; k < 4; k++)
        c.coef[k] = (uint8_t)bits(f0, 6u * (unsigned)k, 6);
    for (int k = 0; k < 3; k++)
        c.coef[4 + k] = (uint8_t)bits(f1, 6u * (unsigned)k, 6);
    c.gamma = bits(trig, 7, 2);
    c.y_scale = scale;
    const uint32_t read = rows ? (rows - 1) * stride + (bytes_per_row < stride ? bytes_per_row : stride) : 0;
    if (!staging(gx, max_u32(gx_xfbenc_size(&c), read))) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: copia al XFB demasiado grande (paso %u, %u filas)", (unsigned)stride, (unsigned)rows);
        return;
    }
    gx_xfbenc(gx->staging, gx->xfb_source, GX_EFB_COLOR(gx->efb), &c);
    staging_to_ram(gx, ram, stride, bytes_per_row, rows);
}

void gc_gx_efb_copy(gc_gx_t *gx)
{
    const uint32_t trig = gx->bp[BP_COPY];
    const uint32_t dest = gx->bp[BP_COPY_DST] << 5, stride = gx->bp[BP_COPY_STRIDE] << 5;
    const uint32_t xy = gx->bp[BP_COPY_SRC_XY], wh = gx->bp[BP_COPY_SRC_WH];
    const int raw_l = (int)bits(xy, 0, 10), raw_t = (int)bits(xy, 10, 10);
    const int raw_r = raw_l + (int)bits(wh, 0, 10) + 1, raw_b = raw_t + (int)bits(wh, 10, 10) + 1;
    int r = raw_r, b = raw_b;
    char line[160];
    const int n = snprintf(line, sizeof line, "c %u %06x src %d %d %d %d dst %08x %u",
                           (unsigned)bits(gx->bp[BP_ZCONTROL], 0, 3), (unsigned)(trig & 0xFFFFFFu),
                           raw_l, raw_t, raw_r - raw_l, raw_b - raw_t, (unsigned)dest, (unsigned)stride);

    if (raw_r > GX_EFB_WIDTH || raw_b > GX_EFB_HEIGHT) {
        if (raw_l >= GX_EFB_WIDTH || raw_t >= GX_EFB_HEIGHT) {
            snprintf(line + n, sizeof line - (size_t)n, " fuera");
            log_line(gx, line);
            if (bits(trig, 14, 1))
                gx->bbox.active = 0;                /* como el camino normal al XFB */
            return;                                 /* ni copia ni borrado */
        }
        if (r > GX_EFB_WIDTH)
            r = GX_EFB_WIDTH;
        if (b > GX_EFB_HEIGHT)
            b = GX_EFB_HEIGHT;
    }

    uint32_t bytes_per_row = 0, rows = 0;
    if (stride != 0) {
        const uint32_t w = (uint32_t)(r - raw_l), h = (uint32_t)(b - raw_t);
        const int to_xfb = (int)bits(trig, 14, 1);
        float scale = 1.0f;
        if (!to_xfb) {                              /* a textura */
            const uint32_t tp = bits(trig, 3, 4);
            copy_size(tp / 2 + (tp & 1u) * 8, (int)bits(trig, 9, 1), w, h, &bytes_per_row, &rows);
        } else {                                    /* al XFB: apaga la caja envolvente */
            gx->bbox.active = 0;
            const uint32_t ys = gx->bp[BP_COPY_YSCALE];
            scale = bits(trig, 10, 1) ? 256.0f / (float)ys : (float)ys / 256.0f;
            const float lines = 1.0f + (float)bits(wh, 10, 10) * scale;
            copy_size(EFB_COPY_XFB, 0, w, gx_x86_f2u(lines), &bytes_per_row, &rows);
        }
        /* GetPointerForRange: fuera de la RAM no se copia nada */
        uint8_t *ram = gc_ram_phys(&gx->sys->mem, dest, rows * stride);
        if (!ram) {
            if (gx->warnings++ < LOG_MAX)
                GC_LOGW("gx: copia del EFB a 0x%08x fuera de la RAM", (unsigned)dest);
        } else if (!to_xfb) {
            texture_copy(gx, trig, raw_l, raw_t, ram, stride, bytes_per_row, rows);
        } else {
            xfb_copy(gx, trig, raw_l, raw_t, r, b, scale, ram, stride, bytes_per_row, rows);
            gx->xfb_copies++;
        }
    }

    if (gx->copy_cb) {
        uint32_t c, z, crc = 0;
        gx_efb_crc(gx->efb, raw_l, raw_t, r, b, &c, &z);
        for (uint32_t k = 0; k < rows; k++) {
            const uint8_t *p = gc_ram_phys(&gx->sys->mem, dest + k * stride, bytes_per_row);
            if (p)
                crc = gc_crc32(crc, p, bytes_per_row);
        }
        snprintf(line + n, sizeof line - (size_t)n, " efb %08x %08x ram %u %08x", (unsigned)c,
                 (unsigned)z, (unsigned)(bytes_per_row * rows), (unsigned)crc);
        log_line(gx, line);
    }

    if (bits(trig, 11, 1)) {
        clear(gx);
        gx_raster_scissor_changed(gx);              /* "Scissor rect must be restored" */
    }
}

uint32_t gc_gx_efb_peek(gc_gx_t *gx, uint32_t phys)
{
    const uint32_t x = (phys & 0xFFFu) >> 2, y = (phys >> 12) & 0x3FFu;
    if (phys & 0x00800000u)
        return 0;                                   /* Z + color: Dolphin no lo emula */
    if (x >= GX_EFB_WIDTH || y >= GX_EFB_HEIGHT)
        return 0;
    const uint32_t pf = bits(gx->bp[BP_ZCONTROL], 0, 3);
    if (phys & 0x00400000u)
        return gx_efb_get_depth(gx->efb, pf, x, y);
    const uint32_t c = gx_efb_get_color(gx->efb, pf, x, y);
    const uint32_t argb = c >> 8 | (c & 0xFFu) << 24;
    switch (gx->sys->pe.alpharead & 3u) {           /* GX_PokeAlphaRead */
    case 1: return argb | 0xFF000000u;
    case 2: return argb;
    default: return argb & 0x00FFFFFFu;
    }
}

void gc_gx_efb_final(gc_gx_t *gx)
{
    char line[48];
    uint32_t c, z;
    gx_efb_crc(gx->efb, 0, 0, GX_EFB_WIDTH, GX_EFB_HEIGHT, &c, &z);
    snprintf(line, sizeof line, "efb %08x %08x", (unsigned)c, (unsigned)z);
    log_line(gx, line);
}
