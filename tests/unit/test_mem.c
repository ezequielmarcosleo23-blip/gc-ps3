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
#include "core/mem.h"
#include "test.h"

/* El log del núcleo va a un contador durante estas pruebas. */
static int warnings;
static void count_sink(char level, const char *msg)
{
    (void)msg;
    if (level == 'W')
        warnings++;
}

static gc_mem_t mem;

static int setup(void)
{
    warnings = 0;
    gc_log_set_sink(count_sink);
    return gc_mem_init(&mem);
}

static void teardown(void)
{
    gc_mem_free(&mem);
    gc_log_set_sink(NULL);
}

static void test_init_zeroed(void)
{
    CHECK(setup() == 0);
    int zero = mem.ram[0] == 0 && mem.ram[GC_RAM_ALLOC - 1] == 0 &&
               mem.aram[GC_ARAM_SIZE - 1] == 0 && mem.lcache[GC_LCACHE_SIZE - 1] == 0;
    int aligned = ((uintptr_t)mem.ram & 0xFFFF) == 0;
    teardown();
    CHECK(zero);
    CHECK(aligned);
}

static void test_regions(void)
{
    CHECK(gc_mem_region(0x80000000u) == GC_REGION_RAM);
    CHECK(gc_mem_region(0x817FFFFFu) == GC_REGION_RAM);
    CHECK(gc_mem_region(0xC0003100u) == GC_REGION_RAM);
    CHECK(gc_mem_region(0x00001000u) == GC_REGION_RAM);       /* física */
    CHECK(gc_mem_region(0xCC002000u) == GC_REGION_MMIO);
    CHECK(gc_mem_region(0x0C006400u) == GC_REGION_MMIO);
    CHECK(gc_mem_region(0xE0000000u) == GC_REGION_LCACHE);
    CHECK(gc_mem_region(0xE0004000u) == GC_REGION_NONE);
    CHECK(gc_mem_region(0xC8000000u) == GC_REGION_NONE);       /* EFB: más adelante */
    CHECK(gc_mem_region(0x82000000u) == GC_REGION_NONE);
    CHECK(gc_mem_region(0x40000000u) == GC_REGION_NONE);
}

static void test_big_endian_and_mirrors(void)
{
    CHECK(setup() == 0);
    gc_write32(&mem, 0x80001000u, 0x12345678u);
    int bytes = mem.ram[0x1000] == 0x12 && mem.ram[0x1003] == 0x78;
    uint32_t cached = gc_read32(&mem, 0x80001000u);
    uint32_t uncached = gc_read32(&mem, 0xC0001000u);
    uint32_t phys = gc_read32(&mem, 0x00001000u);
    uint16_t half = gc_read16(&mem, 0x80001002u);
    uint8_t byte = gc_read8(&mem, 0xC0001001u);
    teardown();
    CHECK(bytes);
    CHECK_EQ_U64(cached, 0x12345678u);
    CHECK_EQ_U64(uncached, 0x12345678u);
    CHECK_EQ_U64(phys, 0x12345678u);
    CHECK_EQ_U64(half, 0x5678u);
    CHECK_EQ_U64(byte, 0x34u);
}

static void test_sizes_and_unaligned(void)
{
    CHECK(setup() == 0);
    gc_write64(&mem, 0x80002000u, 0x0102030405060708ull);
    gc_write16(&mem, 0x80002011u, 0xBEEFu);                    /* no alineado */
    gc_write32(&mem, 0x80002023u, 0xCAFEF00Du);
    uint64_t d = gc_read64(&mem, 0x80002000u);
    uint32_t lo = gc_read32(&mem, 0x80002004u);
    uint16_t h = gc_read16(&mem, 0x80002011u);
    uint32_t w = gc_read32(&mem, 0x80002023u);
    uint8_t b = mem.ram[0x2023];
    teardown();
    CHECK_EQ_U64(d, 0x0102030405060708ull);
    CHECK_EQ_U64(lo, 0x05060708u);
    CHECK_EQ_U64(h, 0xBEEFu);
    CHECK_EQ_U64(w, 0xCAFEF00Du);
    CHECK_EQ_U64(b, 0xCAu);
}

static void test_mask_never_faults(void)
{
    CHECK(setup() == 0);
    gc_write32(&mem, 0x81FFFFFCu, 0xA5A5A5A5u);               /* cola del búfer: no falla */
    uint32_t tail = gc_read32(&mem, 0x81FFFFFCu);
    uint32_t outside = gc_read32(&mem, 0x82000000u);          /* fuera: 0, contado */
    gc_write32(&mem, 0xC8000000u, 1);                          /* EFB: ignorado, contado */
    uint32_t bad = mem.bad;
    int warned = warnings;
    teardown();
    CHECK_EQ_U64(tail, 0xA5A5A5A5u);
    CHECK_EQ_U64(outside, 0);
    CHECK_EQ_U64(bad, 2);
    CHECK(warned == 2);
}

static void test_mmio_not_ram(void)
{
    CHECK(setup() == 0);
    gc_write32(&mem, 0xCC002000u, 0xFFFFFFFFu);               /* VI sin manejador */
    int ram_untouched = mem.ram[0x2000] == 0 && mem.ram[0x0C002000 & GC_RAM_MASK] == 0;
    uint32_t unmapped = mem.io.unmapped;
    teardown();
    CHECK(ram_untouched);
    CHECK_EQ_U64(unmapped, 1);
}

static void test_crossing_the_end(void)
{
    CHECK(setup() == 0);
    mem.ram[GC_RAM_MASK - 1] = 0xAB;                          /* 0x81FFFFFE */
    mem.ram[GC_RAM_MASK] = 0xCD;                              /* 0x81FFFFFF */
    uint32_t v = gc_read32(&mem, 0x81FFFFFEu);                /* 2 bytes de RAM + 2 de nada */
    gc_write32(&mem, 0xE0003FFEu, 0x01020304u);               /* 2 en la caché bloqueada, 2 fuera */
    int lc = mem.lcache[0x3FFE] == 0x01 && mem.lcache[0x3FFF] == 0x02;
    uint32_t bad = mem.bad;
    teardown();
    CHECK_EQ_U64(v, 0xABCD0000u);
    CHECK(lc);
    CHECK_EQ_U64(bad, 4);
}

static void test_lcache(void)
{
    CHECK(setup() == 0);
    gc_write32(&mem, 0xE0000010u, 0x11223344u);
    uint32_t v = gc_read32(&mem, 0xE0000010u);
    int in_buffer = mem.lcache[0x10] == 0x11;
    gc_write8(&mem, 0xE0004000u, 1);                           /* fuera de los 16 KB */
    uint32_t bad = mem.bad;
    teardown();
    CHECK_EQ_U64(v, 0x11223344u);
    CHECK(in_buffer);
    CHECK_EQ_U64(bad, 1);
}

static void test_ram_ptr_bounds(void)
{
    CHECK(setup() == 0);
    uint8_t *a = gc_ram_ptr(&mem, 0x80003100u, 0x100);
    uint8_t *b = gc_ram_ptr(&mem, 0x00003100u, 0x100);
    uint8_t *end = gc_ram_ptr(&mem, 0x817FFF00u, 0x100);       /* justo hasta los 24 MB */
    uint8_t *over = gc_ram_ptr(&mem, 0x817FFF00u, 0x101);      /* se pasa */
    uint8_t *io = gc_ram_ptr(&mem, 0xCC000000u, 4);
    uint8_t *base = mem.ram;
    teardown();
    CHECK(a == base + 0x3100 && b == a);
    CHECK(end == base + 0x17FFF00);
    CHECK(over == NULL && io == NULL);
}

const test_case_t mem_tests[] = {
    { "mem: reserva a cero y alineada",          test_init_zeroed },
    { "mem: regiones del mapa",                  test_regions },
    { "mem: big-endian y espejos 0x8/0xC/física", test_big_endian_and_mirrors },
    { "mem: 8/16/32/64 bits y no alineados",     test_sizes_and_unaligned },
    { "mem: máscara sin fallos, inválidos contados", test_mask_never_faults },
    { "mem: registros no tocan la RAM",          test_mmio_not_ram },
    { "mem: acceso que cruza el final",          test_crossing_the_end },
    { "mem: caché bloqueada",                    test_lcache },
    { "mem: punteros para cargadores y DMA",     test_ram_ptr_bounds },
    { NULL, NULL },
};
