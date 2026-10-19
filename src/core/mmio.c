/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
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
#include "core/mmio.h"

void gc_mmio_init(gc_mmio_t *io)
{
    memset(io, 0, sizeof *io);
    memset(io->page, -1, sizeof io->page);
}

int gc_mmio_register(gc_mmio_t *io, const char *name, uint32_t base, uint32_t size,
                     gc_mmio_read_fn read, gc_mmio_write_fn write, void *ctx)
{
    const uint32_t page_size = 1u << GC_MMIO_PAGE;

    if (io->nblocks >= GC_MMIO_MAX_BLOCKS || size == 0 || (base | size) & (page_size - 1) ||
        base >= GC_MMIO_SPAN || size > GC_MMIO_SPAN - base)
        return -1;
    for (uint32_t p = base >> GC_MMIO_PAGE; p < (base + size) >> GC_MMIO_PAGE; p++)
        if (io->page[p] >= 0)
            return -1;

    gc_mmio_block_t *b = &io->blocks[io->nblocks];
    b->name = name;
    b->base = base;
    b->size = size;
    b->read = read;
    b->write = write;
    b->ctx = ctx;
    for (uint32_t p = base >> GC_MMIO_PAGE; p < (base + size) >> GC_MMIO_PAGE; p++)
        io->page[p] = (int8_t)io->nblocks;
    io->nblocks++;
    return 0;
}

static const gc_mmio_block_t *find(const gc_mmio_t *io, uint32_t phys)
{
    const uint32_t off = phys - GC_MMIO_PHYS;
    if (off >= GC_MMIO_SPAN)
        return NULL;
    const int idx = io->page[off >> GC_MMIO_PAGE];
    return idx < 0 ? NULL : &io->blocks[idx];
}

static void unmapped(gc_mmio_t *io, const char *what, uint32_t phys, int size, uint32_t value)
{
    if (io->unmapped++ < GC_MMIO_LOG_MAX)
        GC_LOGW("MMIO sin manejador: %s%d en 0x%08x (valor 0x%08x)", what, size * 8,
                (unsigned)phys, (unsigned)value);
}

uint32_t gc_mmio_read(gc_mmio_t *io, uint32_t phys, int size)
{
    const gc_mmio_block_t *b = find(io, phys);
    if (!b || !b->read) {
        unmapped(io, "lectura", phys, size, 0);
        return 0;
    }
    return b->read(b->ctx, phys - GC_MMIO_PHYS - b->base, size);
}

void gc_mmio_write(gc_mmio_t *io, uint32_t phys, uint32_t value, int size)
{
    const gc_mmio_block_t *b = find(io, phys);
    if (!b || !b->write) {
        unmapped(io, "escritura", phys, size, value);
        return;
    }
    b->write(b->ctx, phys - GC_MMIO_PHYS - b->base, value, size);
}
