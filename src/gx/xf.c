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
 * Cargas de XF: VideoCommon/XFStructs.cpp de Dolphin (LoadXFReg y
 * LoadIndexedXF). La memoria de XF es una sola tabla de palabras: matrices
 * (0x000-0x0FF posición, 0x400-0x45F normales, 0x500-0x5FF post), luces
 * (0x600-0x67F) y registros (0x1000-0x1057). Una carga que pasa del último
 * registro se corta; una indexada lee de la RAM con la base y el paso del
 * arreglo de CP (12 a 15) y no se corta.
 */
#include "core/log.h"
#include "core/system.h"
#include "gx/gx.h"
#include "gx/raster.h"

#define LOG_MAX 16

static inline uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void gc_gx_load_xf(gc_gx_t *gx, uint16_t base, uint32_t count, const uint8_t *data)
{
    uint32_t addr = base;
    if (addr > GC_XF_REGS_END) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: carga de XF en 0x%04x, despues del ultimo registro", (unsigned)addr);
        return;
    }
    uint32_t end = addr + count;                   /* sin incluir */
    if (end > GC_XF_REGS_END) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: carga de XF de 0x%04x (%u palabras) cortada en el ultimo registro",
                    (unsigned)addr, (unsigned)count);
        end = GC_XF_REGS_END;
    }

    gx->xf_dirty = 1;

    /* Memoria: matrices y luces */
    if (addr < GC_XF_REGS_START) {
        const uint32_t mem_base = addr;
        uint32_t n = count;
        if (end > GC_XF_REGS_START) {
            n = GC_XF_REGS_START - addr;
            addr = GC_XF_REGS_START;
        }
        for (uint32_t i = 0; i < n; i++, data += 4)
            gx->xf[mem_base + i] = be32(data);
    }
    /* Registros; el viewport (0x101A-0x101F) cambia la tijera aunque se
     * escriba el mismo valor, y lo lee el dibujo (backend.h) */
    if (addr < 0x1020u && end > 0x101Au)
        gc_gx_flush(gx);
    if (addr >= GC_XF_REGS_START)
        for (; addr < end; addr++, data += 4) {
            if (addr >= 0x101Au && addr <= 0x101Fu)
                gx->raster->scissor_dirty = 1;
            gx->xf[addr] = be32(data);
        }
}

void gc_gx_load_xf_indexed(gc_gx_t *gx, uint32_t array, uint32_t index, uint16_t address,
                           uint32_t size)
{
    const uint32_t src = gx->cp.array_base[array] + gx->cp.array_stride[array] * index;
    const uint8_t *p = gc_ram_phys(&gx->sys->mem, src, size * 4);
    if (!p) {
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: carga indexada de XF desde 0x%08x fuera de la RAM", (unsigned)src);
        return;                                    /* en Dolphin, un puntero nulo */
    }
    gx->xf_dirty = 1;
    if (address < 0x1020u && (uint32_t)address + size > 0x101Au)
        gc_gx_flush(gx);                           /* el viewport, como gc_gx_load_xf */
    for (uint32_t i = 0; i < size; i++)
        gx->xf[address + i] = be32(p + 4 * i);
}
