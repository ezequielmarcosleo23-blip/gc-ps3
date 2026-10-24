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
 * Lector de comandos del FIFO: VideoCommon/OpcodeDecoding.h y .cpp de
 * Dolphin (RunCommand y RunCallback). Los opcodes se comparan enteros (0x09 no
 * es una carga de CP, 0x62 no es de BP); un desconocido consume un byte.
 *
 * | Comando                   | Bytes          | Ciclos            |
 * | NOP (0x00), seguidos      | n              | 6 por byte        |
 * | CP (0x08)                 | 6              | 12                |
 * | XF (0x10)                 | 5 + 4 palabras | 18 + 6 por palabra|
 * | XF indexada (0x20-0x38)   | 5              | 6                 |
 * | Display list (0x40)       | 9              | 6 + su contenido  |
 * | BP (0x61)                 | 5              | 12                |
 * | 0x44 y 0x48 (INVL_VC)     | 1              | 6                 |
 * | Primitiva (0x80-0xBF)     | 3 + n × vértice| 12 por vértice + 6|
 * | Otro                      | 1              | 1                 |
 */
#include "core/log.h"
#include "core/system.h"
#include "gx/gx.h"

#define LOG_MAX 16

static inline uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static inline uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void display_list(gc_gx_t *gx, uint32_t addr, uint32_t size)
{
    gx->cycles += 6;
    if (gx->in_dl) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: display list dentro de otra (0x%08x), se ignora", (unsigned)addr);
        return;
    }
    const uint8_t *p = gc_ram_phys(&gx->sys->mem, addr, size);
    if (!p) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: display list en 0x%08x (%u bytes) fuera de la RAM", (unsigned)addr,
                    (unsigned)size);
        return;
    }
    gx->in_dl = 1;
    gx->display_lists++;
    /* Lo que no entra entero al final de la lista se pierde (Run se detiene) */
    gc_gx_run_commands(gx, p, size);
    gx->in_dl = 0;
}

static void unknown(gc_gx_t *gx, uint8_t op)
{
    if (op == GX_METRICS || op == GX_INVL_VC) {
        gx->cycles += 6;
        return;
    }
    gx->cycles += 1;
    /* Dolphin calla 0x01-0x07 y 0x3F, que el hardware ignora */
    if (gx->unknown++ < LOG_MAX && op > 0x07 && op != 0x3F)
        GC_LOGW("gx: opcode desconocido 0x%02x", (unsigned)op);
}

/* Un comando: bytes consumidos, o 0 si no está entero. */
static uint32_t run_command(gc_gx_t *gx, const uint8_t *d, uint32_t avail)
{
    if (avail < 1)
        return 0;
    const uint8_t op = d[0];

    switch (op) {
    case GX_NOP: {
        uint32_t n = 1;
        while (n < avail && d[n] == GX_NOP)
            n++;
        gx->cycles += 6 * n;
        return n;
    }
    case GX_LOAD_CP:
        if (avail < 6)
            return 0;
        gx->cycles += 12;
        gc_gx_load_cp(gx, d[1], be32(d + 2));
        return 6;
    case GX_LOAD_XF: {
        if (avail < 5)
            return 0;
        const uint32_t cmd = be32(d + 1);
        const uint32_t n = ((cmd >> 16) & 0xFu) + 1;
        if (avail < 5 + 4 * n)
            return 0;
        gx->cycles += 18 + 6 * n;
        gc_gx_load_xf(gx, (uint16_t)cmd, n, d + 5);
        return 5 + 4 * n;
    }
    case GX_LOAD_INDX_A:
    case GX_LOAD_INDX_B:
    case GX_LOAD_INDX_C:
    case GX_LOAD_INDX_D: {
        if (avail < 5)
            return 0;
        const uint32_t v = be32(d + 1);
        gx->cycles += 6;
        gc_gx_load_xf_indexed(gx, op / 8u + 8u, v >> 16, (uint16_t)(v & 0xFFFu),
                              ((v >> 12) & 0xFu) + 1);
        return 5;
    }
    case GX_CALL_DL:
        if (avail < 9)
            return 0;
        display_list(gx, be32(d + 1) & ~31u, be32(d + 5) & ~31u);
        return 9;
    case GX_LOAD_BP:
        if (avail < 5)
            return 0;
        gx->cycles += 12;
        gc_gx_load_bp(gx, d[1], be24(d + 2));
        return 5;
    default:
        break;
    }

    if (op >= GX_PRIMITIVE_FIRST && op <= GX_PRIMITIVE_LAST) {
        if (avail < 3)
            return 0;
        const uint32_t vsize = gc_gx_vertex_size(gx, op & 7u);
        const uint32_t n = be16(d + 1);
        if (avail < 3 + n * vsize)
            return 0;
        /* "4 GPU ticks per vertex, 3 CPU ticks per GPU tick" */
        gx->cycles += n * 4 * 3 + 6;
        gx->primitives++;
        gx->vertices += n;
        return 3 + n * vsize;
    }

    unknown(gx, op);
    return 1;
}

uint32_t gc_gx_run_commands(gc_gx_t *gx, const uint8_t *data, uint32_t avail)
{
    uint32_t used = 0;
    while (used < avail) {
        const uint32_t n = run_command(gx, data + used, avail - used);
        if (n == 0)
            break;
        used += n;
        gx->commands++;
    }
    return used;
}
