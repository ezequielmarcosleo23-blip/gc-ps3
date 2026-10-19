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

/* Arma DOLs en memoria para las pruebas (test_dol.c, test_system.c). */
#ifndef GCPS3_TESTS_DOL_BUILD_H
#define GCPS3_TESTS_DOL_BUILD_H

#include <stdint.h>
#include <string.h>

#include "core/dol.h"

#define DOLB_CAP 0x4000u

typedef struct dolb {
    uint8_t buf[DOLB_CAP];
    size_t  len;
} dolb_t;

static inline void dolb_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void dolb_init(dolb_t *b)
{
    memset(b, 0, sizeof *b);
    b->len = GC_DOL_HEADER_SIZE;
}

/* Agrega la sección s (0-6 texto, 7-17 datos) al final del archivo, alineada
 * a 32 bytes; `bytes` puede ser NULL (relleno con un patrón). */
static inline void dolb_section(dolb_t *b, int s, uint32_t addr, const void *bytes, uint32_t n)
{
    const size_t off = (b->len + 31u) & ~(size_t)31u;
    if (bytes)
        memcpy(b->buf + off, bytes, n);
    else
        for (uint32_t k = 0; k < n; k++)
            b->buf[off + k] = (uint8_t)(0xA0u + s + k);
    dolb_put32(b->buf + 0x00 + 4 * s, (uint32_t)off);
    dolb_put32(b->buf + 0x48 + 4 * s, addr);
    dolb_put32(b->buf + 0x90 + 4 * s, n);
    b->len = off + n;
}

/* Sección de texto con instrucciones (big-endian). */
static inline void dolb_code(dolb_t *b, int s, uint32_t addr, const uint32_t *code, uint32_t count)
{
    uint8_t tmp[1024];
    for (uint32_t k = 0; k < count; k++)
        dolb_put32(tmp + 4 * k, code[k]);
    dolb_section(b, s, addr, tmp, 4 * count);
}

static inline void dolb_bss(dolb_t *b, uint32_t addr, uint32_t size)
{
    dolb_put32(b->buf + 0xD8, addr);
    dolb_put32(b->buf + 0xDC, size);
}

static inline void dolb_entry(dolb_t *b, uint32_t entry)
{
    dolb_put32(b->buf + 0xE0, entry);
}

#endif
