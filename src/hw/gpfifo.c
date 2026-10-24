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
#include "hw/gpfifo.h"

#define LOG_MAX 8

void gc_gpfifo_reset(gc_gpfifo_t *g)
{
    g->count = 0;
}

/* GPFifoManager::UpdateGatherPipe: cada bloque completo va a la RAM en el
 * puntero del PI y avisa al CP. */
static void flush_blocks(gc_gpfifo_t *g)
{
    gc_system_t *s = g->sys;
    gc_pi_t *pi = &s->pi;
    uint32_t done = 0;

    while (g->count - done >= GC_GPFIFO_BLOCK) {
        uint8_t *dst = gc_ram_phys(&s->mem, pi->fifo_wptr, GC_GPFIFO_BLOCK);
        if (dst)
            memcpy(dst, g->buf + done, GC_GPFIFO_BLOCK);
        else if (g->bad_copies++ < LOG_MAX)
            GC_LOGW("gpfifo: bloque a 0x%08x, fuera de la RAM", (unsigned)pi->fifo_wptr);
        done += GC_GPFIFO_BLOCK;

        if (pi->fifo_wptr == pi->fifo_end)
            pi->fifo_wptr = pi->fifo_base;
        else
            pi->fifo_wptr += GC_GPFIFO_BLOCK;

        gc_cp_gather_pipe_bursted(&s->cp);
    }
    memmove(g->buf, g->buf + done, g->count - done);
    g->count -= done;
}

void gc_gpfifo_write(gc_gpfifo_t *g, uint32_t value, int size)
{
    for (int k = size - 1; k >= 0; k--)
        g->buf[g->count++] = (uint8_t)(value >> (8 * k));
    g->bytes += (uint64_t)size;
    if (g->count >= GC_GPFIFO_BLOCK)
        flush_blocks(g);
}

/* ---- registro en el mapa ---------------------------------------------------------- */

static uint32_t pipe_read(void *ctx, uint32_t off, int size)
{
    (void)ctx;
    GC_LOGW("gpfifo: lectura de %d bits en 0x%03x", size * 8, (unsigned)off);
    return 0;
}

static void pipe_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    (void)off;
    gc_gpfifo_write(ctx, v, size);
}

void gc_gpfifo_init(gc_gpfifo_t *g, struct gc_system *sys)
{
    g->sys = sys;
    g->bytes = 0;
    g->bad_copies = 0;
    gc_gpfifo_reset(g);
    gc_mmio_register(&sys->mem.io, "GXFIFO", GC_MMIO_GXFIFO, GC_GPFIFO_SPAN, pipe_read, pipe_write, g);
}
