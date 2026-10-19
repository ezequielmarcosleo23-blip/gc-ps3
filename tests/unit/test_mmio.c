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

typedef struct fake_block {
    uint32_t last_offset;
    uint32_t last_value;
    int      last_size;
    int      reads;
    int      writes;
    uint32_t regs[64];
} fake_block_t;

static uint32_t fake_read(void *ctx, uint32_t offset, int size)
{
    fake_block_t *f = ctx;
    f->reads++;
    f->last_offset = offset;
    f->last_size = size;
    return f->regs[(offset / 4) % 64] >> (size == 2 && (offset & 2) == 0 ? 16 : 0);
}

static void fake_write(void *ctx, uint32_t offset, uint32_t value, int size)
{
    fake_block_t *f = ctx;
    f->writes++;
    f->last_offset = offset;
    f->last_value = value;
    f->last_size = size;
    if (size == 4)
        f->regs[(offset / 4) % 64] = value;
}

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void test_register_rules(void)
{
    gc_mmio_t io;
    gc_mmio_init(&io);
    CHECK(gc_mmio_register(&io, "VI", GC_MMIO_VI, 0x100, NULL, NULL, NULL) == 0);
    CHECK(gc_mmio_register(&io, "otro", GC_MMIO_VI, 0x100, NULL, NULL, NULL) == -1); /* se pisan */
    CHECK(gc_mmio_register(&io, "mal", 0x2010, 0x100, NULL, NULL, NULL) == -1);      /* sin alinear */
    CHECK(gc_mmio_register(&io, "fuera", 0xFF00, 0x200, NULL, NULL, NULL) == -1);    /* se sale */
    CHECK(gc_mmio_register(&io, "PI", GC_MMIO_PI, 0x100, NULL, NULL, NULL) == 0);
    CHECK(io.nblocks == 2);
}

static void test_dispatch(void)
{
    static gc_mem_t mem;
    static fake_block_t vi;
    memset(&vi, 0, sizeof vi);
    gc_log_set_sink(quiet);
    CHECK(gc_mem_init(&mem) == 0);
    int reg = gc_mmio_register(&mem.io, "VI", GC_MMIO_VI, 0x100, fake_read, fake_write, &vi);

    gc_write32(&mem, 0xCC00201Cu, 0xDEADBEEFu);               /* VI + 0x1C */
    uint32_t off_w = vi.last_offset, val_w = vi.last_value;
    int size_w = vi.last_size;
    uint16_t hi = gc_read16(&mem, 0xCC00201Cu);                /* mitad alta */
    uint32_t off_r = vi.last_offset;
    int size_r = vi.last_size;
    uint64_t d = gc_read64(&mem, 0x0C002018u);                 /* física, dos lecturas de 32 */
    int reads = vi.reads;
    uint32_t unmapped = mem.io.unmapped;
    gc_mem_free(&mem);
    gc_log_set_sink(NULL);

    CHECK(reg == 0);
    CHECK_EQ_U64(off_w, 0x1C);
    CHECK_EQ_U64(val_w, 0xDEADBEEFu);
    CHECK(size_w == 4);
    CHECK_EQ_U64(hi, 0xDEADu);
    CHECK_EQ_U64(off_r, 0x1C);
    CHECK(size_r == 2);
    CHECK_EQ_U64(d, 0x00000000DEADBEEFull);
    CHECK(reads == 3);
    CHECK_EQ_U64(unmapped, 0);
}

static int warns;
static void count(char level, const char *msg)
{
    (void)msg;
    warns += level == 'W';
}

static void test_unmapped_logged_then_counted(void)
{
    gc_mmio_t io;
    gc_mmio_init(&io);
    warns = 0;
    gc_log_set_sink(count);
    uint32_t v = 1;
    for (int i = 0; i < GC_MMIO_LOG_MAX + 10; i++)
        v |= gc_mmio_read(&io, GC_MMIO_PHYS + GC_MMIO_AI, 4);
    gc_mmio_write(&io, GC_MMIO_PHYS + GC_MMIO_GXFIFO, 0x61, 1);
    gc_log_set_sink(NULL);
    CHECK_EQ_U64(v, 1);                                          /* siempre leyó 0 */
    CHECK_EQ_U64(io.unmapped, GC_MMIO_LOG_MAX + 11);
    CHECK(warns == GC_MMIO_LOG_MAX);
}

const test_case_t mmio_tests[] = {
    { "mmio: reglas de registro",               test_register_rules },
    { "mmio: despacho con desplazamiento y tamaño", test_dispatch },
    { "mmio: sin manejador, log y después solo cuenta", test_unmapped_logged_then_counted },
    { NULL, NULL },
};
