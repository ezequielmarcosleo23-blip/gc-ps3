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

#include "core/endian.h"
#include "test.h"

static void test_load_big_endian(void)
{
    const uint8_t mem[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    CHECK_EQ_U64(be16_load(mem), 0x0123);
    CHECK_EQ_U64(be32_load(mem), 0x01234567);
    CHECK_EQ_U64(be64_load(mem), 0x0123456789abcdefULL);
}

static void test_store_big_endian(void)
{
    uint8_t mem[8] = { 0 };
    be32_store(mem, 0x80003100u);   /* dirección típica de la RAM de GameCube */
    CHECK(mem[0] == 0x80 && mem[1] == 0x00 && mem[2] == 0x31 && mem[3] == 0x00);
    be16_store(mem + 4, 0xbeef);
    CHECK(mem[4] == 0xbe && mem[5] == 0xef);
}

static void test_unaligned_roundtrip(void)
{
    uint8_t buf[16];
    memset(buf, 0xcc, sizeof buf);
    be64_store(buf + 3, 0xfedcba9876543210ULL);   /* dirección impar a propósito */
    CHECK_EQ_U64(be64_load(buf + 3), 0xfedcba9876543210ULL);
    CHECK(buf[2] == 0xcc && buf[11] == 0xcc);     /* no pisa los vecinos */
}

const test_case_t endian_tests[] = {
    { "endian: lectura big-endian",       test_load_big_endian },
    { "endian: escritura big-endian",     test_store_big_endian },
    { "endian: ida y vuelta no alineada", test_unaligned_roundtrip },
    { NULL, NULL },
};
