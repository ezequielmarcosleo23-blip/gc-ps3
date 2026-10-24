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
 * Estado de CP que llega por el FIFO (comando 0x08): VideoCommon/CPMemory.cpp
 * de Dolphin (LoadCPReg, FillCPMemoryArray) y el tamaño de un vértice en el
 * FIFO (VertexLoaderBase::GetVertexSize con las tablas de VertexLoader_*.h).
 * Se elige el registro por los 4 bits de arriba: 0x31 también es MATINDEX_A,
 * 0x78 es el grupo A del formato 0.
 */
#include "core/log.h"
#include "core/system.h"
#include "gx/gx.h"

#define LOG_MAX 16

void gc_gx_load_cp(gc_gx_t *gx, uint8_t cmd, uint32_t value)
{
    gc_gx_cp_state_t *s = &gx->cp;
    switch (cmd & 0xF0u) {
    case 0x00:
    case 0x10:
    case 0x20:
        return;                    /* sin uso conocido (consultas de rendimiento); 0x20 = 0 al iniciar */
    case GX_CP_MATINDEX_A: s->matindex_a = value; return;
    case GX_CP_MATINDEX_B: s->matindex_b = value; return;
    case GX_CP_VCD_LO:     s->vcd_lo = value; return;
    case GX_CP_VCD_HI:     s->vcd_hi = value; return;
    case GX_CP_VAT_A:      s->vat[cmd & 7u][0] = value; return;
    case GX_CP_VAT_B:      s->vat[cmd & 7u][1] = value; return;
    case GX_CP_VAT_C:      s->vat[cmd & 7u][2] = value; return;
    case GX_CP_ARRAY_BASE:   s->array_base[cmd & 0xFu] = value & GC_CP_ADDR_MASK; return;
    case GX_CP_ARRAY_STRIDE: s->array_stride[cmd & 0xFu] = value & 0xFFu; return;
    default:
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: registro de CP 0x%02x desconocido (0x%08x)", (unsigned)cmd, (unsigned)value);
        return;
    }
}

void gc_gx_cp_memory(const gc_gx_t *gx, uint32_t out[256])
{
    const gc_gx_cp_state_t *s = &gx->cp;
    for (int k = 0; k < 256; k++)
        out[k] = 0;
    out[GX_CP_MATINDEX_A] = s->matindex_a;
    out[GX_CP_MATINDEX_B] = s->matindex_b;
    out[GX_CP_VCD_LO] = s->vcd_lo;
    out[GX_CP_VCD_HI] = s->vcd_hi;
    for (int i = 0; i < 8; i++) {
        out[GX_CP_VAT_A + i] = s->vat[i][0];
        out[GX_CP_VAT_B + i] = s->vat[i][1];
        out[GX_CP_VAT_C + i] = s->vat[i][2];
    }
    for (int i = 0; i < 16; i++) {
        out[GX_CP_ARRAY_BASE + i] = s->array_base[i];
        out[GX_CP_ARRAY_STRIDE + i] = s->array_stride[i];
    }
}

/* ---- tamaño de un vértice ------------------------------------------------------------- */

/* Formato de un componente en el VCD: 0 no está, 1 directo, 2 índice de 8 bits,
 * 3 de 16. Los formatos 5 a 7 (inválidos) miden como float. */
enum { NOT_PRESENT = 0, DIRECT = 1, INDEX8 = 2, INDEX16 = 3 };

static const uint8_t comp_bytes[8] = { 1, 1, 2, 2, 4, 4, 4, 4 };   /* u8 s8 u16 s16 float */

static uint32_t index_size(uint32_t type)
{
    return type == INDEX8 ? 1 : type == INDEX16 ? 2 : 0;
}

/* VertexLoader_Position: 2 o 3 componentes */
static uint32_t position_size(uint32_t type, uint32_t format, uint32_t elements)
{
    if (type == DIRECT)
        return comp_bytes[format] * (elements ? 3u : 2u);
    return index_size(type);
}

/* VertexLoader_Normal: N (3) o NTB (9); con NormalIndex3 y NTB, un índice por vector */
static uint32_t normal_size(uint32_t type, uint32_t format, uint32_t elements, uint32_t index3)
{
    if (type == DIRECT)
        return comp_bytes[format] * (elements ? 9u : 3u);
    return index_size(type) * (index3 && elements ? 3u : 1u);
}

/* VertexLoader_Color: 565, 888, 888x, 4444, 6666, 8888 */
static uint32_t color_size(gc_gx_t *gx, uint32_t type, uint32_t format)
{
    static const uint8_t direct[6] = { 2, 3, 4, 2, 3, 4 };
    if (format > 5) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: formato de color %u invalido en el VAT", (unsigned)format);
        return 0;                  /* Dolphin: PanicAlert y 0 */
    }
    if (type == DIRECT)
        return direct[format];
    return index_size(type);
}

/* VertexLoader_TextCoord: 1 o 2 componentes */
static uint32_t texcoord_size(uint32_t type, uint32_t format, uint32_t elements)
{
    if (type == DIRECT)
        return comp_bytes[format] * (elements ? 2u : 1u);
    return index_size(type);
}

static uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

uint32_t gc_gx_vertex_size(gc_gx_t *gx, uint32_t vat)
{
    const gc_gx_cp_state_t *s = &gx->cp;
    const uint32_t lo = s->vcd_lo, hi = s->vcd_hi;
    const uint32_t g0 = s->vat[vat][0], g1 = s->vat[vat][1], g2 = s->vat[vat][2];
    uint32_t size = 0;

    /* PosMatIdx y los 8 TexMatIdx: un byte cada uno */
    for (unsigned k = 0; k < 9; k++)
        size += bits(lo, k, 1);

    size += position_size(bits(lo, 9, 2), bits(g0, 1, 3), bits(g0, 0, 1));
    size += normal_size(bits(lo, 11, 2), bits(g0, 10, 3), bits(g0, 9, 1), bits(g0, 31, 1));
    size += color_size(gx, bits(lo, 13, 2), bits(g0, 14, 3));
    size += color_size(gx, bits(lo, 15, 2), bits(g0, 18, 3));

    /* Coordenadas de textura: elementos y formato de cada una en g0, g1 y g2 */
    static const struct { uint8_t group, elem_at, fmt_at; } tc[8] = {
        { 0, 21, 22 }, { 1, 0, 1 }, { 1, 9, 10 }, { 1, 18, 19 },
        { 1, 27, 28 }, { 2, 5, 6 }, { 2, 14, 15 }, { 2, 23, 24 },
    };
    const uint32_t g[3] = { g0, g1, g2 };
    for (unsigned k = 0; k < 8; k++)
        size += texcoord_size(bits(hi, 2 * k, 2), bits(g[tc[k].group], tc[k].fmt_at, 3),
                              bits(g[tc[k].group], tc[k].elem_at, 1));
    return size;
}
