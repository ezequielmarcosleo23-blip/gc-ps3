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

/*
 * Acceso big-endian a la memoria emulada.
 *
 * La GameCube y la PS3 son big-endian, así que en la consola estas funciones
 * son una copia directa; en el runner de PC (x86, little-endian) invierten bytes.
 * Usan memcpy para permitir direcciones no alineadas sin comportamiento indefinido.
 */
#ifndef GCPS3_CORE_ENDIAN_H
#define GCPS3_CORE_ENDIAN_H

#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define GCPS3_HOST_BIG_ENDIAN 1
#else
#define GCPS3_HOST_BIG_ENDIAN 0
#endif

static inline uint16_t gc_bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t gc_bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t gc_bswap64(uint64_t v) { return __builtin_bswap64(v); }

static inline uint16_t be16_load(const void *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return GCPS3_HOST_BIG_ENDIAN ? v : gc_bswap16(v);
}

static inline uint32_t be32_load(const void *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return GCPS3_HOST_BIG_ENDIAN ? v : gc_bswap32(v);
}

static inline uint64_t be64_load(const void *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return GCPS3_HOST_BIG_ENDIAN ? v : gc_bswap64(v);
}

static inline void be16_store(void *p, uint16_t v)
{
    v = GCPS3_HOST_BIG_ENDIAN ? v : gc_bswap16(v);
    memcpy(p, &v, sizeof v);
}

static inline void be32_store(void *p, uint32_t v)
{
    v = GCPS3_HOST_BIG_ENDIAN ? v : gc_bswap32(v);
    memcpy(p, &v, sizeof v);
}

static inline void be64_store(void *p, uint64_t v)
{
    v = GCPS3_HOST_BIG_ENDIAN ? v : gc_bswap64(v);
    memcpy(p, &v, sizeof v);
}

#endif
