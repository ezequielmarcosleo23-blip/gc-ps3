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
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "hw/mi.h"

/* Los registros que existen: regiones y tipo de protección (0x00-0x10),
 * máscara e indicadores de interrupción, dirección del fallo (0x1C-0x24) y
 * los diez contadores (0x32-0x5A). */
static int valid(uint32_t off)
{
    return off <= 0x10 || (off >= 0x1C && off <= 0x24) || (off >= 0x32 && off <= 0x5A);
}

static uint16_t read_reg(gc_mi_t *mi, uint32_t off)
{
    if (!valid(off)) {
        GC_LOGW("mi: lectura de 0x%02x sin registro", (unsigned)off);
        return 0;
    }
    return mi->reg[off / 2];
}

static void write_reg(gc_mi_t *mi, uint32_t off, uint16_t v)
{
    if (!valid(off)) {
        GC_LOGW("mi: escritura a 0x%02x sin registro ignorada", (unsigned)off);
        return;
    }
    mi->reg[off / 2] = v;
}

static uint32_t mi_read(void *ctx, uint32_t off, int size)
{
    gc_mi_t *mi = ctx;
    if (size == 4)
        return (uint32_t)read_reg(mi, off & ~3u) << 16 | read_reg(mi, (off & ~3u) + 2);
    if (size == 2)
        return read_reg(mi, off & ~1u);
    GC_LOGW("mi: lectura de 8 bits en 0x%02x", (unsigned)off);
    return 0;
}

static void mi_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_mi_t *mi = ctx;
    if (size == 4) {
        write_reg(mi, off & ~3u, (uint16_t)(v >> 16));
        write_reg(mi, (off & ~3u) + 2, (uint16_t)v);
    } else if (size == 2) {
        write_reg(mi, off & ~1u, (uint16_t)v);
    } else {
        GC_LOGW("mi: escritura de 8 bits en 0x%02x ignorada", (unsigned)off);
    }
}

void gc_mi_reset(gc_mi_t *mi)
{
    memset(mi->reg, 0, sizeof mi->reg);
}

void gc_mi_init(gc_mi_t *mi, struct gc_system *sys)
{
    mi->sys = sys;
    gc_mi_reset(mi);
    gc_mmio_register(&sys->mem.io, "MI", GC_MMIO_MI, 0x100, mi_read, mi_write, mi);
}
