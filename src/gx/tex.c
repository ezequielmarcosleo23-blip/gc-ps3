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
 * Texturas (Fase 2, etapa C4): la TMEM y sus cargas por BP (BPStructs.cpp) y
 * el muestreador del renderizador por software de Dolphin cf5cb7ef
 * (TextureSampler.cpp): envoltura, filtro más cercano y bilineal en s17.7,
 * mipmaps más cercano y lineal. Sin caché: cada texel se lee de la RAM o de la
 * TMEM y se decodifica en el momento (texdec.c).
 *
 * Como Dolphin:
 *   - el comienzo de cada mipmap se calcula con el tamaño sin redondear al
 *     bloque (mipWidth × mipHeight × nibbles / 2, con mínimo de un bloque);
 *   - en RGBA8 precargado solo avanza el banco par al bajar de mipmap;
 *   - la envoltura 3 es como recorte (en Dolphin, además un PanicAlert).
 * En un formato de textura que no existe Dolphin deja el texel sin escribir
 * (basura de la pila); acá queda en 0.
 */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/gx.h"
#include "gx/tev.h"
#include "gx/texdec.h"

enum {
    BP_PRELOAD_ADDR = 0x60, BP_PRELOAD_EVEN = 0x61, BP_PRELOAD_ODD = 0x62, BP_PRELOAD_MODE = 0x63,
    BP_TLUT_SRC = 0x64, BP_TLUT_DEST = 0x65,
};

#define LOG_MAX 16

static inline uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

/* Los registros de la unidad de textura texmap (TexUnitAddress) */
static inline uint32_t unit_reg(const gc_gx_t *gx, uint32_t texmap, uint32_t reg)
{
    return gx->bp[0x80u | (texmap & 3u) | reg << 2 | (texmap & 4u) << 3];
}
enum { R_MODE0 = 0, R_MODE1 = 1, R_IMAGE0 = 2, R_IMAGE1 = 3, R_IMAGE2 = 4, R_IMAGE3 = 5, R_TLUT = 6 };

/* ---- TMEM ------------------------------------------------------------------------------- */

/* Memory::CopyFromEmu: si el tramo no es RAM, no copia nada (Dolphin avisa) */
static void copy_from_ram(gc_gx_t *gx, uint32_t tmem_off, uint32_t addr, uint32_t n)
{
    if (!n)
        return;
    const uint8_t *p = gc_ram_phys(&gx->sys->mem, addr, n);
    if (!p) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: carga de TMEM desde 0x%08x (%u bytes) fuera de la RAM", (unsigned)addr, (unsigned)n);
        return;
    }
    memcpy(gx->tmem + tmem_off, p, n);
}

void gc_gx_load_tlut(gc_gx_t *gx)
{
    const uint32_t dest = gx->bp[BP_TLUT_DEST];
    const uint32_t tmem = bits(dest, 0, 10) << 9;
    const uint32_t count = bits(dest, 10, 11) * GX_TMEM_LINE_SIZE;
    /* La GameCube no mira los bits de arriba de la dirección */
    const uint32_t addr = (gx->bp[BP_TLUT_SRC] << 5) & 0x01FFFFFFu;
    copy_from_ram(gx, tmem, addr, count);           /* siempre entra: 0x7FE00 + 0xFFE0 < 1 MB */
}

void gc_gx_preload(gc_gx_t *gx)
{
    if (gx->bp[BP_PRELOAD_MODE] == 0)
        return;
    const uint32_t info = gx->bp[BP_PRELOAD_MODE];
    const uint32_t count = bits(info, 0, 15);
    const uint32_t src = gx->bp[BP_PRELOAD_ADDR] << 5;
    uint32_t even = gx->bp[BP_PRELOAD_EVEN] * GX_TMEM_LINE_SIZE;

    if (bits(info, 15, 2) != 3) {
        if (even < GX_TMEM_SIZE) {
            uint32_t n = count * GX_TMEM_LINE_SIZE;
            if (even + n > GX_TMEM_SIZE)
                n = GX_TMEM_SIZE - even;
            copy_from_ram(gx, even, src, n);
        }
        return;
    }
    /* RGBA8: los bloques de alfa y rojo al banco par, los de verde y azul al impar */
    uint32_t odd = gx->bp[BP_PRELOAD_ODD] * GX_TMEM_LINE_SIZE, read = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (even + GX_TMEM_LINE_SIZE > GX_TMEM_SIZE || odd + GX_TMEM_LINE_SIZE > GX_TMEM_SIZE)
            break;
        copy_from_ram(gx, even, src + read, GX_TMEM_LINE_SIZE);
        copy_from_ram(gx, odd, src + read + GX_TMEM_LINE_SIZE, GX_TMEM_LINE_SIZE);
        even += GX_TMEM_LINE_SIZE;
        odd += GX_TMEM_LINE_SIZE;
        read += 2 * GX_TMEM_LINE_SIZE;
    }
}

/* TexDecoder_GetTmemSpan */
static gx_span_t tmem_span(const gc_gx_t *gx, uint32_t off)
{
    gx_span_t s;
    if (off > GX_TMEM_SIZE)
        off = GX_TMEM_SIZE;
    s.p = gx->tmem + off;
    s.n = GX_TMEM_SIZE - off;
    return s;
}

/* Memory::GetSpanForAddress: desde la dirección hasta el final de la RAM */
static gx_span_t ram_span(gc_gx_t *gx, uint32_t addr)
{
    gx_span_t s = { NULL, 0 };
    addr &= 0x3FFFFFFFu;
    if (addr < GC_RAM_SIZE) {
        s.p = gx->sys->mem.ram + addr;
        s.n = GC_RAM_SIZE - addr;
    } else if (gx->warnings++ < LOG_MAX) {
        GC_LOGW("gx: textura en 0x%08x, fuera de la RAM", (unsigned)addr);
    }
    return s;
}

static gx_span_t subspan(gx_span_t s, uint32_t off)
{
    if (off > s.n)
        off = s.n;
    s.p += off;
    s.n -= off;
    return s;
}

/* ---- muestreo (TextureSampler) ---------------------------------------------------------- */

static int32_t wrap_coord(int32_t c, uint32_t mode, int32_t size)
{
    switch (mode) {
    case 1:                                     /* repetir: con AND, también sin potencia de 2 */
        return c & (size - 1);
    case 2:                                     /* espejo */
        if (c & size)
            c = ~c;
        return c & (size - 1);
    default:                                    /* recortar (y el 3, inválido) */
        return c < 0 ? 0 : c > size - 1 ? size - 1 : c;
    }
}

static inline void set_texel(const uint8_t in[4], uint32_t acc[4], uint32_t f)
{
    for (int i = 0; i < 4; i++)
        acc[i] = in[i] * f;
}

static inline void add_texel(const uint8_t in[4], uint32_t acc[4], uint32_t f)
{
    for (int i = 0; i < 4; i++)
        acc[i] += in[i] * f;
}

static void sample_mip(gc_gx_t *gx, int32_t s, int32_t t, int32_t mip, int linear, uint32_t texmap,
                       uint8_t out[4])
{
    const uint32_t tm0 = unit_reg(gx, texmap, R_MODE0), ti0 = unit_reg(gx, texmap, R_IMAGE0);
    const uint32_t ti1 = unit_reg(gx, texmap, R_IMAGE1), tlut_reg = unit_reg(gx, texmap, R_TLUT);
    const uint32_t fmt = bits(ti0, 20, 4), tlutfmt = bits(tlut_reg, 10, 2);
    const int tmem = (int)bits(ti1, 21, 1);
    const int rgba8_tmem = tmem && fmt == GX_TF_RGBA8;

    gx_span_t src, src_odd = { NULL, 0 };
    if (tmem) {
        src = tmem_span(gx, bits(ti1, 0, 15) * GX_TMEM_LINE_SIZE);
        if (fmt == GX_TF_RGBA8)
            src_odd = tmem_span(gx, bits(unit_reg(gx, texmap, R_IMAGE2), 0, 15) * GX_TMEM_LINE_SIZE);
    } else {
        src = ram_span(gx, bits(unit_reg(gx, texmap, R_IMAGE3), 0, 24) << 5);
    }
    int32_t w_m1 = (int32_t)bits(ti0, 0, 10), h_m1 = (int32_t)bits(ti0, 10, 10);
    const gx_span_t tlut = tmem_span(gx, bits(tlut_reg, 0, 10) << 9);

    memset(out, 0, 4);
    if (!gx_tex_valid_format(fmt)) {
        if (!gx->tex_noted) {
            gx->tex_noted = 1;
            GC_LOGW("gx: textura con formato %u, que no existe", (unsigned)fmt);
        }
        return;
    }

    /* Al mipmap: tamaño, coordenadas y comienzo */
    if (mip) {
        int32_t mw = w_m1 + 1, mh = h_m1 + 1;
        const int32_t fw = (int32_t)gx_tex_block_width(fmt), fh = (int32_t)gx_tex_block_height(fmt);
        const int32_t depth = (int32_t)gx_tex_nibbles(fmt);
        w_m1 >>= mip;
        h_m1 >>= mip;
        s >>= mip;
        t >>= mip;
        for (int32_t m = mip; m; m--) {
            if (mw < fw)
                mw = fw;
            if (mh < fh)
                mh = fh;
            src = subspan(src, (uint32_t)((mw * mh * depth) >> 1));
            mw >>= 1;
            mh >>= 1;
        }
    }

    if (linear) {
        s -= 64;
        t -= 64;
        int32_t is = s >> 7, it = t >> 7, is1 = is + 1, it1 = it + 1;
        const uint32_t fs = (uint32_t)s & 0x7Fu, ft = (uint32_t)t & 0x7Fu;
        is = wrap_coord(is, bits(tm0, 0, 2), w_m1 + 1);
        it = wrap_coord(it, bits(tm0, 2, 2), h_m1 + 1);
        is1 = wrap_coord(is1, bits(tm0, 0, 2), w_m1 + 1);
        it1 = wrap_coord(it1, bits(tm0, 2, 2), h_m1 + 1);
        uint8_t texel[4] = { 0, 0, 0, 0 };
        uint32_t acc[4];
#define FETCH(S, T)                                                                         \
        do {                                                                                \
            if (rgba8_tmem)                                                                 \
                gx_tex_decode_texel_rgba8_tmem(texel, src, src_odd, S, T, w_m1);            \
            else                                                                            \
                gx_tex_decode_texel(texel, src, S, T, w_m1, fmt, tlut, tlutfmt);            \
        } while (0)
        FETCH(is, it);
        set_texel(texel, acc, (128 - fs) * (128 - ft));
        FETCH(is1, it);
        add_texel(texel, acc, fs * (128 - ft));
        FETCH(is, it1);
        add_texel(texel, acc, (128 - fs) * ft);
        FETCH(is1, it1);
        add_texel(texel, acc, fs * ft);
#undef FETCH
        for (int i = 0; i < 4; i++)
            out[i] = (uint8_t)(acc[i] >> 14);
    } else {
        const int32_t is = wrap_coord(s >> 7, bits(tm0, 0, 2), w_m1 + 1);
        const int32_t it = wrap_coord(t >> 7, bits(tm0, 2, 2), h_m1 + 1);
        if (rgba8_tmem)
            gx_tex_decode_texel_rgba8_tmem(out, src, src_odd, is, it, w_m1);
        else
            gx_tex_decode_texel(out, src, is, it, w_m1, fmt, tlut, tlutfmt);
    }
}

void gx_tex_sample(gc_gx_t *gx, int32_t s, int32_t t, int32_t lod, int linear, uint32_t texmap,
                   uint8_t out[4])
{
    const uint32_t tm0 = unit_reg(gx, texmap, R_MODE0);
    const uint32_t mipmode = bits(tm0, 5, 2);
    const int32_t frac = lod & 0xF;
    int32_t base = 0;
    int mip_linear = 0;
    if (lod > 0 && mipmode != 0) {
        base = lod >> 4;
        mip_linear = frac && mipmode == 2;
        if (mipmode == 1 && frac >= 8)          /* más cercano: desde ,5 el siguiente */
            base++;
    }

    if (mip_linear) {
        uint8_t texel[4];
        uint32_t acc[4];
        sample_mip(gx, s, t, base, linear, texmap, texel);
        set_texel(texel, acc, (uint32_t)(16 - frac));
        sample_mip(gx, s, t, base + 1, linear, texmap, texel);
        add_texel(texel, acc, (uint32_t)frac);
        for (int i = 0; i < 4; i++)
            out[i] = (uint8_t)(acc[i] >> 4);
    } else {
        sample_mip(gx, s, t, base, linear, texmap, out);
    }
}
