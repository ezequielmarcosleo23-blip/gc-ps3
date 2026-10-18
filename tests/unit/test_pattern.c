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

#include "core/pattern.h"
#include "test.h"

/* Los mismos bytes los comprueba tools/mkiso.py --selftest. */
static void test_fixture(void)
{
    static const uint8_t expected[8] = { 0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x7c };
    uint8_t buf[8];
    pattern_fill(buf, 0x12345678, sizeof buf);
    CHECK(memcmp(buf, expected, sizeof buf) == 0);
}

static void test_roundtrip_and_wrap(void)
{
    uint8_t buf[64];
    pattern_fill(buf, 0xFFFFFFF0ull, sizeof buf);          /* cruza los 4 GB */
    CHECK(pattern_check(buf, 0xFFFFFFF0ull, sizeof buf) == -1);
    CHECK(pattern_check(buf, 0x1FFFFFFF0ull, sizeof buf) == -1); /* solo 32 bits bajos */
    CHECK(buf[16] == 0 && buf[19] == 0);                   /* la palabra en 4 GB vale 0 */
}

static void test_detects_corruption(void)
{
    enum { N = 32768 };
    uint8_t *buf = malloc(N);
    CHECK(buf != NULL);
    pattern_fill(buf, PATTERN_GC_DISC_SIZE - N, N);         /* el último trozo del disco */
    CHECK(pattern_check(buf, PATTERN_GC_DISC_SIZE - N, N) == -1);
    buf[12345] ^= 0x01;
    long at = pattern_check(buf, PATTERN_GC_DISC_SIZE - N, N);
    free(buf);
    CHECK(at == 12344);
}

static void test_detects_wrong_offset(void)
{
    uint8_t buf[2048];
    pattern_fill(buf, 4096, sizeof buf);                   /* un sector de más */
    CHECK(pattern_check(buf, 2048, sizeof buf) == 0);
}

const test_case_t pattern_tests[] = {
    { "pattern: bytes de referencia",      test_fixture },
    { "pattern: ida y vuelta, cruza 4 GB",  test_roundtrip_and_wrap },
    { "pattern: detecta un bit cambiado",  test_detects_corruption },
    { "pattern: detecta posición errónea", test_detects_wrong_offset },
    { NULL, NULL },
};
