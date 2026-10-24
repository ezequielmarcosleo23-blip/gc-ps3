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
#include <stdlib.h>
#include <string.h>

#include "core/endian.h"
#include "core/log.h"
#include "core/mem.h"

/* malloc con alineación propia: memalign/aligned_alloc no están en todas las libc. */
static uint8_t *alloc_aligned(size_t size, size_t align, void **raw)
{
    *raw = malloc(size + align);
    if (!*raw)
        return NULL;
    uintptr_t p = ((uintptr_t)*raw + align - 1) & ~(uintptr_t)(align - 1);
    memset((void *)p, 0, size);
    return (uint8_t *)p;
}

int gc_mem_init(gc_mem_t *m)
{
    memset(m, 0, sizeof *m);
    m->ram = alloc_aligned(GC_RAM_ALLOC, 0x10000, &m->raw[0]);
    m->aram = alloc_aligned(GC_ARAM_SIZE, 128, &m->raw[1]);
    m->lcache = alloc_aligned(GC_LCACHE_SIZE, 128, &m->raw[2]);
    gc_mmio_init(&m->io);
    if (!m->ram || !m->aram || !m->lcache) {
        gc_mem_free(m);
        return -1;
    }
    return 0;
}

void gc_mem_free(gc_mem_t *m)
{
    for (int i = 0; i < 3; i++)
        free(m->raw[i]);
    memset(m, 0, sizeof *m);
}

gc_region_t gc_mem_region(uint32_t addr)
{
    const uint32_t seg = addr >> 28;

    if (seg == 0x0 || seg == 0x8 || seg == 0xC) {
        const uint32_t phys = gc_mem_phys(addr);
        if (phys < GC_RAM_ALLOC)
            return GC_REGION_RAM;
        if (phys - GC_MMIO_PHYS < GC_MMIO_SPAN)
            return GC_REGION_MMIO;
        return GC_REGION_NONE;
    }
    if (addr - GC_LCACHE_BASE < GC_LCACHE_SIZE)
        return GC_REGION_LCACHE;
    return GC_REGION_NONE;
}

static void bad_access(gc_mem_t *m, const char *what, uint32_t addr, int size)
{
    if (m->bad++ < GC_LOG_BAD_MAX)
        GC_LOGW("acceso a 0x%08x sin memoria ni registros (%s%d)", (unsigned)addr, what, size * 8);
}

/* Puntero al primer byte, o NULL si la dirección no es RAM ni caché bloqueada. */
static inline uint8_t *host_ptr(gc_mem_t *m, uint32_t addr, gc_region_t r)
{
    if (r == GC_REGION_RAM)
        return m->ram + (addr & GC_RAM_MASK);
    if (r == GC_REGION_LCACHE)
        return m->lcache + (addr - GC_LCACHE_BASE);
    return NULL;
}

/* Un acceso que se sale del final de la RAM o de la caché bloqueada (no
 * alineado justo en el borde) se arma byte a byte con la dirección siguiente. */
static inline int fits(uint32_t addr, gc_region_t r, int size)
{
    if (r == GC_REGION_RAM)
        return (addr & GC_RAM_MASK) <= GC_RAM_ALLOC - (uint32_t)size;
    return addr - GC_LCACHE_BASE <= GC_LCACHE_SIZE - (uint32_t)size;
}

static uint32_t read_n(gc_mem_t *m, uint32_t addr, int size)
{
    const gc_region_t r = gc_mem_region(addr);
    uint8_t *p = host_ptr(m, addr, r);

    if (p && fits(addr, r, size)) {
        switch (size) {
        case 1: return p[0];
        case 2: return be16_load(p);
        default: return be32_load(p);
        }
    }
    if (p) {                                   /* cruza el final: byte a byte */
        uint32_t v = 0;
        for (int i = 0; i < size; i++)
            v = (v << 8) | gc_read8(m, addr + (uint32_t)i);
        return v;
    }
    if (r == GC_REGION_MMIO)
        return gc_mmio_read(&m->io, gc_mem_phys(addr), size);
    bad_access(m, "lectura", addr, size);
    return 0;
}

static void write_n(gc_mem_t *m, uint32_t addr, uint32_t v, int size)
{
    const gc_region_t r = gc_mem_region(addr);
    uint8_t *p = host_ptr(m, addr, r);

    if (p && fits(addr, r, size)) {
        switch (size) {
        case 1: p[0] = (uint8_t)v; return;
        case 2: be16_store(p, (uint16_t)v); return;
        default: be32_store(p, v); return;
        }
    }
    if (p) {
        for (int i = 0; i < size; i++)
            gc_write8(m, addr + (uint32_t)i, (uint8_t)(v >> (8 * (size - 1 - i))));
        return;
    }
    if (r == GC_REGION_MMIO) {
        gc_mmio_write(&m->io, gc_mem_phys(addr), v, size);
        return;
    }
    bad_access(m, "escritura", addr, size);
}

uint8_t gc_read8(gc_mem_t *m, uint32_t addr)
{
    const gc_region_t r = gc_mem_region(addr);
    uint8_t *p = host_ptr(m, addr, r);
    return p ? *p : (uint8_t)read_n(m, addr, 1);
}

uint16_t gc_read16(gc_mem_t *m, uint32_t addr) { return (uint16_t)read_n(m, addr, 2); }
uint32_t gc_read32(gc_mem_t *m, uint32_t addr) { return read_n(m, addr, 4); }

/* 64 bits = dos de 32 (palabra alta primero), también en los registros. */
uint64_t gc_read64(gc_mem_t *m, uint32_t addr)
{
    const uint64_t hi = read_n(m, addr, 4);
    return (hi << 32) | read_n(m, addr + 4, 4);
}

void gc_write8(gc_mem_t *m, uint32_t addr, uint8_t v)
{
    const gc_region_t r = gc_mem_region(addr);
    uint8_t *p = host_ptr(m, addr, r);
    if (p)
        *p = v;
    else
        write_n(m, addr, v, 1);
}

void gc_write16(gc_mem_t *m, uint32_t addr, uint16_t v) { write_n(m, addr, v, 2); }
void gc_write32(gc_mem_t *m, uint32_t addr, uint32_t v) { write_n(m, addr, v, 4); }

void gc_write64(gc_mem_t *m, uint32_t addr, uint64_t v)
{
    write_n(m, addr, (uint32_t)(v >> 32), 4);
    write_n(m, addr + 4, (uint32_t)v, 4);
}

uint8_t *gc_ram_ptr(gc_mem_t *m, uint32_t addr, uint32_t len)
{
    if (gc_mem_region(addr) != GC_REGION_RAM)
        return NULL;
    const uint32_t phys = gc_mem_phys(addr);
    if (phys >= GC_RAM_SIZE || len > GC_RAM_SIZE - phys)
        return NULL;
    return m->ram + phys;
}

uint8_t *gc_ram_phys(gc_mem_t *m, uint32_t addr, uint32_t len)
{
    addr &= 0x3FFFFFFFu;
    if (addr >= GC_RAM_SIZE || len > GC_RAM_SIZE - addr)
        return NULL;
    return m->ram + addr;
}
