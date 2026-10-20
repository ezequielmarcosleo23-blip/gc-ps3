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
#include "hw/gx.h"

/* ---- registros de 16 bits (los de 32 son dos, la mitad alta primero) --------------- */

typedef uint16_t (*read16_fn)(gc_gx_t *gx, uint32_t off);
typedef void (*write16_fn)(gc_gx_t *gx, uint32_t off, uint16_t v);

static uint32_t read_sized(gc_gx_t *gx, read16_fn rd, uint32_t off, int size, const char *who)
{
    if (size == 4)
        return (uint32_t)rd(gx, off & ~3u) << 16 | rd(gx, (off & ~3u) + 2);
    if (size == 2)
        return rd(gx, off & ~1u);
    GC_LOGW("%s: lectura de 8 bits en 0x%02x", who, (unsigned)off);
    return 0;
}

static void write_sized(gc_gx_t *gx, write16_fn wr, uint32_t off, uint32_t v, int size,
                        const char *who)
{
    if (size == 4) {
        wr(gx, off & ~3u, (uint16_t)(v >> 16));
        wr(gx, (off & ~3u) + 2, (uint16_t)v);
    } else if (size == 2) {
        wr(gx, off & ~1u, (uint16_t)v);
    } else {
        GC_LOGW("%s: escritura de 8 bits en 0x%02x ignorada", who, (unsigned)off);
    }
}

/* ---- Command Processor ------------------------------------------------------------ */

static uint16_t cp_read16(gc_gx_t *gx, uint32_t off)
{
    if (off >= GC_CP_REGS_SIZE)
        return 0;
    if (off == CP_SR)
        return CP_SR_IDLE;                         /* sin GX: siempre ocioso */
    return gx->cp[off / 2];
}

static void cp_write16(gc_gx_t *gx, uint32_t off, uint16_t v)
{
    if (off >= GC_CP_REGS_SIZE || off == CP_SR) {
        GC_LOGW("cp: escritura a 0x%02x ignorada", (unsigned)off);
        return;
    }
    if (off >= CP_FIFO_FIRST && off <= CP_FIFO_LAST)   /* direcciones físicas de 32 bytes */
        v &= (off & 2) ? 0x03FFu : 0xFFE0u;
    gx->cp[off / 2] = v;
}

static uint32_t cp_read(void *ctx, uint32_t off, int size)
{
    return read_sized(ctx, cp_read16, off, size, "cp");
}

static void cp_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    write_sized(ctx, cp_write16, off, v, size, "cp");
}

/* ---- Pixel Engine ------------------------------------------------------------------ */

static uint16_t pe_read16(gc_gx_t *gx, uint32_t off)
{
    if (off >= GC_PE_REGS_SIZE)
        return 0;
    return gx->pe[off / 2];
}

static void pe_write16(gc_gx_t *gx, uint32_t off, uint16_t v)
{
    if (off >= GC_PE_REGS_SIZE || off == PE_TOKEN) {
        GC_LOGW("pe: escritura a 0x%02x ignorada", (unsigned)off);
        return;
    }
    if (off == PE_CTRL)
        v &= 0x0003u;                              /* habilitaciones; TOKEN y FINISH solo se escriben */
    gx->pe[off / 2] = v;
}

static uint32_t pe_read(void *ctx, uint32_t off, int size)
{
    return read_sized(ctx, pe_read16, off, size, "pe");
}

static void pe_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    write_sized(ctx, pe_write16, off, v, size, "pe");
}

/* ---- FIFO de GX ------------------------------------------------------------------- */

static uint32_t fifo_read(void *ctx, uint32_t off, int size)
{
    (void)ctx;
    GC_LOGW("gx: lectura de %d bits del FIFO (0x%02x)", size * 8, (unsigned)off);
    return 0;
}

static void fifo_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_gx_t *gx = ctx;
    (void)off;
    (void)v;
    gx->fifo_bytes += (uint64_t)size;
}

void gc_gx_reset(gc_gx_t *gx)
{
    memset(gx->cp, 0, sizeof gx->cp);
    memset(gx->pe, 0, sizeof gx->pe);
    gx->fifo_bytes = 0;
}

void gc_gx_init(gc_gx_t *gx, struct gc_system *sys)
{
    gx->sys = sys;
    gc_gx_reset(gx);
    gc_mmio_register(&sys->mem.io, "CP", GC_MMIO_CP, 0x100, cp_read, cp_write, gx);
    gc_mmio_register(&sys->mem.io, "PE", GC_MMIO_PE, 0x100, pe_read, pe_write, gx);
    gc_mmio_register(&sys->mem.io, "GXFIFO", GC_MMIO_GXFIFO, 0x100, fifo_read, fifo_write, gx);
}
