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
 * Cargador de vértices (ver vtx.h). gx_vtx_setup y gx_vtx_gather corren en el
 * PPU; gx_vtx_decode no toca nada fuera de sus argumentos (listo para SPU).
 */
#include <string.h>

#include "gx/vtx.h"

/* ---- campos de VCD y VAT (CPMemory.h) -------------------------------------------- */

static uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

/* Bytes de un componente: u8 s8 u16 s16 float (5-7: float, como Dolphin) */
static const uint8_t comp_bytes[8] = { 1, 1, 2, 2, 4, 4, 4, 4 };

/* Bytes de un color directo: 565 888 888x 4444 6666 8888 */
static const uint8_t color_bytes[8] = { 2, 3, 4, 2, 3, 4, 0, 0 };

/* Exponente de la escala de las normales enteras (VertexLoaderX64: SCALE_MAP) */
static const uint8_t normal_exp[8] = { 7, 6, 15, 14, 0, 0, 0, 0 };

static float pow2_neg(unsigned e)
{
    return 1.0f / (float)(1u << e);
}

/* Elementos, formato y parte fraccionaria de la coordenada i en los grupos A, B, C */
static const struct { uint8_t group, elem_at, fmt_at, frac_at; } tc_fields[8] = {
    { 0, 21, 22, 25 }, { 1, 0, 1, 4 },    { 1, 9, 10, 13 },  { 1, 18, 19, 22 },
    { 1, 27, 28, 0 },  { 2, 5, 6, 9 },    { 2, 14, 15, 18 }, { 2, 23, 24, 27 },
};

static uint32_t tex_frac(const uint32_t vat[3], unsigned i)
{
    if (i == 4)                                     /* Tex4Frac está en el grupo C */
        return bits(vat[2], 0, 5);
    return bits(vat[tc_fields[i].group], tc_fields[i].frac_at, 5);
}

static void add_step(gx_vtx_plan_t *p, uint8_t type, uint8_t array, uint8_t parts, uint8_t is_pos,
                     uint16_t bytes, uint16_t *off)
{
    gx_vtx_step_t *s = &p->step[p->nsteps++];
    s->type = type;
    s->array = array;
    s->parts = parts;
    s->is_pos = is_pos;
    s->bytes = bytes;
    *off = (uint16_t)(*off + bytes);
    if (type == GX_VTX_DIRECT_ATTR)
        p->raw_size += bytes;
    else
        p->raw_size += (type == GX_VTX_INDEX8 ? 1u : 2u) * parts;
}

static void no_attr(gx_vtx_attr_t *a)
{
    a->off = -1;
    a->fmt = 0;
    a->count = 0;
    a->scale = 0.0f;
}

void gx_vtx_setup(const uint32_t vcd[2], const uint32_t vat[3], gx_vtx_plan_t *p)
{
    const uint32_t lo = vcd[0], hi = vcd[1], g0 = vat[0];
    const int dequant = (int)bits(g0, 30, 1);
    gx_vtx_desc_t *d = &p->desc;
    uint16_t off = 0;

    memset(p, 0, sizeof *p);
    d->posmtx = -1;
    for (int i = 0; i < 8; i++)
        d->texmtx[i] = -1;
    no_attr(&d->pos);
    no_attr(&d->nrm);
    no_attr(&d->col[0]);
    no_attr(&d->col[1]);
    for (int i = 0; i < 8; i++)
        no_attr(&d->tex[i]);

    /* Índices de matriz: un byte cada uno */
    if (bits(lo, 0, 1)) {
        d->posmtx = (int16_t)off;
        add_step(p, GX_VTX_DIRECT_ATTR, 0, 1, 0, 1, &off);
    }
    for (unsigned i = 0; i < 8; i++)
        if (bits(lo, 1 + i, 1)) {
            d->texmtx[i] = (int16_t)off;
            add_step(p, GX_VTX_DIRECT_ATTR, 0, 1, 0, 1, &off);
        }

    /* Posición */
    const uint32_t pos_type = bits(lo, 9, 2);
    if (pos_type != GX_VTX_NONE) {
        const uint32_t fmt = bits(g0, 1, 3), frac = bits(g0, 4, 5);
        const uint8_t count = bits(g0, 0, 1) ? 3 : 2;
        d->pos.off = (int16_t)off;
        d->pos.fmt = (uint8_t)fmt;
        d->pos.count = count;
        d->pos.scale = fmt < 4 && dequant && frac ? pow2_neg(frac) : 0.0f;
        if (pos_type != GX_VTX_DIRECT_ATTR) {
            p->pos_ix_bytes = pos_type == GX_VTX_INDEX8 ? 1 : 2;
            p->pos_ix_at = (uint16_t)p->raw_size;
        }
        add_step(p, (uint8_t)pos_type, 0, 1, 1, (uint16_t)(comp_bytes[fmt] * count), &off);
    }

    /* Normal, o normal, tangente y binormal */
    const uint32_t nrm_type = bits(lo, 11, 2);
    if (nrm_type != GX_VTX_NONE) {
        const uint32_t fmt = bits(g0, 10, 3);
        const int ntb = (int)bits(g0, 9, 1);
        const int index3 = (int)bits(g0, 31, 1);
        const uint8_t count = ntb ? 9 : 3;
        const uint8_t parts = ntb && index3 && nrm_type != GX_VTX_DIRECT_ATTR ? 3 : 1;
        d->nrm.off = (int16_t)off;
        d->nrm.fmt = (uint8_t)fmt;
        d->nrm.count = count;
        d->nrm.scale = normal_exp[fmt] ? pow2_neg(normal_exp[fmt]) : 0.0f;
        add_step(p, (uint8_t)nrm_type, 1, parts, 0, (uint16_t)(comp_bytes[fmt] * count), &off);
        p->has_nrm = 1;
        p->has_ntb = (uint8_t)ntb;
    }

    /* Colores */
    for (unsigned i = 0; i < 2; i++) {
        const uint32_t type = bits(lo, 13 + 2 * i, 2);
        if (type == GX_VTX_NONE)
            continue;
        const uint32_t fmt = bits(g0, 14 + 4 * i, 3);
        d->col[i].off = (int16_t)off;
        d->col[i].fmt = (uint8_t)fmt;
        d->col[i].count = 4;
        add_step(p, (uint8_t)type, (uint8_t)(2 + i), 1, 0, color_bytes[fmt], &off);
    }

    /* Coordenadas de textura */
    for (unsigned i = 0; i < 8; i++) {
        const uint32_t type = bits(hi, 2 * i, 2);
        if (type == GX_VTX_NONE)
            continue;
        const uint32_t g = vat[tc_fields[i].group];
        const uint32_t fmt = bits(g, tc_fields[i].fmt_at, 3), frac = tex_frac(vat, i);
        const uint8_t count = bits(g, tc_fields[i].elem_at, 1) ? 2 : 1;
        d->tex[i].off = (int16_t)off;
        d->tex[i].fmt = (uint8_t)fmt;
        d->tex[i].count = count;
        d->tex[i].scale = fmt < 4 && dequant && frac ? pow2_neg(frac) : 0.0f;
        add_step(p, (uint8_t)type, (uint8_t)(4 + i), 1, 0, (uint16_t)(comp_bytes[fmt] * count), &off);
    }

    d->stride = off;
}

/* ---- juntar (PPU) ------------------------------------------------------------------- */

uint32_t gx_vtx_gather(const gx_vtx_plan_t *p, const uint32_t base[16], const uint32_t stride[16],
                       gx_vtx_ram_fn ram, void *ram_ctx, const uint8_t *raw, uint32_t n,
                       uint8_t *dst, uint32_t *bad_reads)
{
    uint32_t out = 0;
    for (uint32_t v = 0; v < n; v++) {
        const uint8_t *r = raw + (size_t)v * p->raw_size;
        uint8_t *d = dst + (size_t)out * p->desc.stride;
        int skip = 0;

        for (uint32_t k = 0; k < p->nsteps; k++) {
            const gx_vtx_step_t *s = &p->step[k];
            if (s->type == GX_VTX_DIRECT_ATTR) {
                memcpy(d, r, s->bytes);
                r += s->bytes;
                d += s->bytes;
                continue;
            }
            const uint32_t part = s->bytes / s->parts;
            for (uint32_t j = 0; j < s->parts; j++) {
                uint32_t idx;
                if (s->type == GX_VTX_INDEX8) {
                    idx = r[0];
                    r += 1;
                    if (s->is_pos && idx == 0xFFu)
                        skip = 1;
                } else {
                    idx = (uint32_t)r[0] << 8 | r[1];
                    r += 2;
                    if (s->is_pos && idx == 0xFFFFu)
                        skip = 1;
                }
                /* Con tres índices, cada uno toma su tercio de la entrada */
                const uint32_t addr = base[s->array] + stride[s->array] * idx + part * j;
                const uint8_t *src = ram(ram_ctx, addr, part);
                if (src) {
                    memcpy(d, src, part);
                } else {
                    memset(d, 0, part);
                    if (bad_reads)
                        (*bad_reads)++;
                }
                d += part;
            }
        }
        if (!skip)
            out++;
    }
    return out;
}

uint32_t gx_vtx_count_loaded(const gx_vtx_plan_t *p, const uint8_t *raw, uint32_t n)
{
    if (!p->pos_ix_bytes)
        return n;
    uint32_t loaded = 0;
    for (uint32_t v = 0; v < n; v++) {
        const uint8_t *r = raw + (size_t)v * p->raw_size + p->pos_ix_at;
        const int skip = p->pos_ix_bytes == 1 ? r[0] == 0xFFu : (r[0] == 0xFFu && r[1] == 0xFFu);
        loaded += !skip;
    }
    return loaded;
}

/* ---- decodificar (listo para SPU) --------------------------------------------------- */

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return be24(p) << 8 | p[3]; }

static float as_float(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* Un componente como lo deja el cargador x64: entero a float (CVTDQ2PS) y, si
 * hay escala, multiplicado por ella (MULPS); float tal cual. */
static float component(const uint8_t *p, uint32_t fmt, float scale)
{
    float v;
    switch (fmt) {
    case 0: v = (float)p[0]; break;
    case 1: v = (float)(int8_t)p[0]; break;
    case 2: v = (float)be16(p); break;
    case 3: v = (float)(int16_t)be16(p); break;
    default: return as_float(be32(p));
    }
    return scale != 0.0f ? v * scale : v;
}

static uint8_t expand4(uint32_t c) { return (uint8_t)(c << 4 | c); }
static uint8_t expand5(uint32_t c) { return (uint8_t)(c << 3 | c >> 2); }
static uint8_t expand6(uint32_t c) { return (uint8_t)(c << 2 | c >> 4); }

/* VertexLoader_Color: a RGBA de 8 bits */
static void color(const uint8_t *p, uint32_t fmt, uint8_t rgba[4])
{
    uint32_t v;
    switch (fmt) {
    case 0:                                         /* RGB565 */
        v = be16(p);
        rgba[0] = expand5(v >> 11);
        rgba[1] = expand6((v >> 5) & 63u);
        rgba[2] = expand5(v & 31u);
        rgba[3] = 255;
        return;
    case 1:                                         /* RGB888 */
    case 2:                                         /* RGB888x */
        rgba[0] = p[0];
        rgba[1] = p[1];
        rgba[2] = p[2];
        rgba[3] = 255;
        return;
    case 3:                                         /* RGBA4444 */
        v = be16(p);
        rgba[0] = expand4(v >> 12);
        rgba[1] = expand4((v >> 8) & 15u);
        rgba[2] = expand4((v >> 4) & 15u);
        rgba[3] = expand4(v & 15u);
        return;
    case 4:                                         /* RGBA6666 */
        v = be24(p);
        rgba[0] = expand6(v >> 18);
        rgba[1] = expand6((v >> 12) & 63u);
        rgba[2] = expand6((v >> 6) & 63u);
        rgba[3] = expand6(v & 63u);
        return;
    case 5:                                         /* RGBA8888 */
        memcpy(rgba, p, 4);
        return;
    default:                                        /* inválido: Dolphin no lo lee */
        memset(rgba, 255, 4);
        return;
    }
}

void gx_vtx_decode(const gx_vtx_desc_t *d, const gx_vtx_defaults_t *def, const uint8_t *src,
                   uint32_t n, gx_vtx_in_t *out)
{
    for (uint32_t v = 0; v < n; v++, src += d->stride) {
        gx_vtx_in_t *o = &out[v];
        memset(o, 0, sizeof *o);

        o->posmtx = d->posmtx >= 0 ? (uint8_t)(src[d->posmtx] & 0x3Fu) : def->posmtx;
        for (int i = 0; i < 8; i++)
            o->texmtx[i] = d->texmtx[i] >= 0 ? src[d->texmtx[i]] : def->texmtx[i];

        if (d->pos.off >= 0) {
            const uint32_t cb = comp_bytes[d->pos.fmt];
            for (uint32_t c = 0; c < d->pos.count; c++)
                o->pos[c] = component(src + d->pos.off + cb * c, d->pos.fmt, d->pos.scale);
        }

        memcpy(o->nrm, def->nrm, sizeof o->nrm);
        if (d->nrm.off >= 0) {
            const uint32_t cb = comp_bytes[d->nrm.fmt];
            for (uint32_t c = 0; c < d->nrm.count; c++)
                o->nrm[c / 3][c % 3] = component(src + d->nrm.off + cb * c, d->nrm.fmt, d->nrm.scale);
        }

        /* Sin el color 0, el 1 va al canal 0 (ParseColorAttributes) */
        uint8_t col[2][4];
        memset(col, 255, sizeof col);
        int present[2] = { d->col[0].off >= 0, d->col[1].off >= 0 };
        for (int i = 0; i < 2; i++)
            if (present[i])
                color(src + d->col[i].off, d->col[i].fmt, col[i]);
        memset(o->col, 255, sizeof o->col);
        if (present[0]) {
            memcpy(o->col[0], col[0], 4);
            if (present[1])
                memcpy(o->col[1], col[1], 4);
        } else if (present[1]) {
            memcpy(o->col[0], col[1], 4);
        }

        for (int i = 0; i < 8; i++) {
            const gx_vtx_attr_t *t = &d->tex[i];
            if (t->off < 0)
                continue;
            const uint32_t cb = comp_bytes[t->fmt];
            o->tex[i][0] = component(src + t->off, t->fmt, t->scale);
            if (t->count == 2)
                o->tex[i][1] = component(src + t->off + cb, t->fmt, t->scale);
        }
    }
}
