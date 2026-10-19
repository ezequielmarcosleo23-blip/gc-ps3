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

/* Lector de DOL: archivos válidos y cada forma de archivo roto. */
#include "core/dol.h"
#include "core/log.h"
#include "dol_build.h"
#include "test.h"

#define TEXT0 0x80003100u
#define DATA0 0x80010000u

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

/* DOL mínimo válido: texto en 0x80003100, datos de 40 bytes, BSS. */
static void valid(dolb_t *b)
{
    static const uint32_t code[] = { 0x38600005u, 0x48000000u };     /* li r3,5; b . */
    dolb_init(b);
    dolb_code(b, 0, TEXT0, code, 2);
    dolb_section(b, 7, DATA0, NULL, 40);
    dolb_section(b, 8, 0xC0020000u, NULL, 64);                         /* sin caché: misma RAM */
    dolb_bss(b, 0x80030000u, 0x1000u);
    dolb_entry(b, TEXT0);
}

static void test_parse_and_load(void)
{
    static dolb_t b;
    static gc_mem_t mem;
    gc_dol_t dol;
    int bad = 99;

    valid(&b);
    CHECK(gc_dol_parse(b.buf, b.len, &dol, &bad) == GC_DOL_OK);
    CHECK(bad == -1);
    CHECK(dol.entry == TEXT0 && dol.addr[7] == DATA0 && dol.size[7] == 40);
    CHECK(dol.bss_addr == 0x80030000u && dol.bss_size == 0x1000u);

    gc_log_set_sink(quiet);
    CHECK(gc_mem_init(&mem) == 0);
    gc_write32(&mem, 0x80030000u, 0x12345678u);                    /* el BSS no se borra */
    gc_dol_load(&dol, b.buf, b.len, &mem);
    CHECK(gc_read32(&mem, TEXT0) == 0x38600005u && gc_read32(&mem, TEXT0 + 4) == 0x48000000u);
    CHECK(gc_read8(&mem, DATA0) == 0xA7 && gc_read8(&mem, DATA0 + 39) == (uint8_t)(0xA7 + 39));
    /* la sección 7 se copió redondeada a 32 bytes (lo que sigue en el archivo) */
    CHECK(gc_read8(&mem, DATA0 + 40) == b.buf[dol.offset[7] + 40]);
    CHECK(gc_read8(&mem, 0x80020000u + 63) == (uint8_t)(0xA8 + 63));
    CHECK(gc_read32(&mem, 0x80030000u) == 0x12345678u);
    gc_mem_free(&mem);
    gc_log_set_sink(NULL);
}

static void put32(dolb_t *b, uint32_t at, uint32_t v)
{
    dolb_put32(b->buf + at, v);
}

static void test_broken(void)
{
    static dolb_t b;
    gc_dol_t dol;
    int bad;

    CHECK(gc_dol_parse(b.buf, 0xFF, &dol, &bad) == GC_DOL_ERR_SHORT);

    valid(&b);                                     /* la sección 8 sale del archivo */
    CHECK(gc_dol_parse(b.buf, b.len - 20, &dol, &bad) == GC_DOL_ERR_FILE);
    CHECK(bad == 8);

    valid(&b);                                     /* datos que pasan de 24 MB */
    put32(&b, 0x48 + 4 * 7, 0x817FFFF0u);
    CHECK(gc_dol_parse(b.buf, b.len, &dol, &bad) == GC_DOL_ERR_RAM);
    CHECK(bad == 7);

    valid(&b);                                     /* dirección que no es RAM */
    put32(&b, 0x48 + 4 * 7, 0x90000000u);
    CHECK(gc_dol_parse(b.buf, b.len, &dol, &bad) == GC_DOL_ERR_RAM);

    valid(&b);                                     /* 0xC0010010 pisa 0x80010000 (alias) */
    put32(&b, 0x48 + 4 * 8, 0xC0010010u);
    CHECK(gc_dol_parse(b.buf, b.len, &dol, &bad) == GC_DOL_ERR_OVERLAP);
    CHECK(bad == 8);

    valid(&b);                                     /* entrada en los datos */
    dolb_entry(&b, DATA0);
    CHECK(gc_dol_parse(b.buf, b.len, &dol, &bad) == GC_DOL_ERR_ENTRY);

    valid(&b);                                     /* BSS fuera de la RAM */
    dolb_bss(&b, 0x81700000u, 0x00200000u);
    CHECK(gc_dol_parse(b.buf, b.len, &dol, &bad) == GC_DOL_ERR_RAM);
    CHECK(bad == -1);

    valid(&b);                                     /* sin texto */
    put32(&b, 0x90, 0);
    CHECK(gc_dol_parse(b.buf, b.len, &dol, &bad) == GC_DOL_ERR_EMPTY);

    static const uint32_t wii[] = { 0x7CB3FBA6u, 0x48000000u };   /* mtspr HID4, r5 */
    dolb_init(&b);
    dolb_code(&b, 0, TEXT0, wii, 2);
    dolb_entry(&b, TEXT0);
    CHECK(gc_dol_parse(b.buf, b.len, &dol, &bad) == GC_DOL_ERR_WII);
    CHECK(gc_dol_strerror(GC_DOL_ERR_WII)[0] != '\0');
}

const test_case_t dol_tests[] = {
    { "dol: lee y carga secciones (32 bytes, alias sin cache, BSS intacto)", test_parse_and_load },
    { "dol: archivos rotos y de Wii", test_broken },
    { NULL, NULL },
};
