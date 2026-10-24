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
 * Con copy_cb, una línea por copia como la del parche de Dolphin
 * (GCPS3_COPIAS): formato de píxel, BP 0x52, rectángulo, destino, CRC del EFB
 * (después de copiar y antes de borrar) y de los bytes que quedaron en la RAM.
 */
#include <stdio.h>

#include "core/crc32.h"
#include "core/log.h"
#include "core/system.h"
#include "gx/efb.h"
#include "gx/gx.h"

#define LOG_MAX 16

enum {
    BP_ZMODE = 0x40, BP_BLENDMODE = 0x41, BP_ZCONTROL = 0x43, BP_COPY_SRC_XY = 0x49,
    BP_COPY_SRC_WH = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D, BP_COPY_YSCALE = 0x4E,
    BP_CLEAR_AR = 0x4F, BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51, BP_COPY = 0x52,
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

/* static_cast<u32>(float) de x86-64: cvttss2si de 64 bits y los 32 de abajo;
 * NaN o fuera de rango da 0 */
static uint32_t float_to_u32(float f)
{
    if (!(f > -9223372036854775808.0f && f < 9223372036854775808.0f))
        return 0;
    return (uint32_t)(uint64_t)(int64_t)f;
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
        if (!bits(trig, 14, 1)) {                   /* a textura */
            const uint32_t tp = bits(trig, 3, 4);
            copy_size(tp / 2 + (tp & 1u) * 8, (int)bits(trig, 9, 1), w, h, &bytes_per_row, &rows);
        } else {                                    /* al XFB */
            const uint32_t ys = gx->bp[BP_COPY_YSCALE];
            const float scale = bits(trig, 10, 1) ? 256.0f / (float)ys : (float)ys / 256.0f;
            const float lines = 1.0f + (float)bits(wh, 10, 10) * scale;
            copy_size(EFB_COPY_XFB, 0, w, float_to_u32(lines), &bytes_per_row, &rows);
        }
        /* La conversión a la RAM llega en el paso C6 */
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: copia del EFB a 0x%08x todavia sin escribir en la RAM", (unsigned)dest);
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

    if (bits(trig, 11, 1))
        clear(gx);
}

void gc_gx_efb_final(gc_gx_t *gx)
{
    char line[48];
    uint32_t c, z;
    gx_efb_crc(gx->efb, 0, 0, GX_EFB_WIDTH, GX_EFB_HEIGHT, &c, &z);
    snprintf(line, sizeof line, "efb %08x %08x", (unsigned)c, (unsigned)z);
    log_line(gx, line);
}
