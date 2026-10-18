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
#include "core/endian.h"
#include "core/pattern.h"

void pattern_fill(void *buf, uint64_t offset, size_t len)
{
    uint8_t *p = buf;
    uint32_t v = (uint32_t)offset;
    for (size_t i = 0; i + 4 <= len; i += 4, v += 4)
        be32_store(p + i, v);
}

long pattern_check(const void *buf, uint64_t offset, size_t len)
{
    const uint8_t *p = buf;
    uint32_t v = (uint32_t)offset;
    for (size_t i = 0; i + 4 <= len; i += 4, v += 4)
        if (be32_load(p + i) != v)
            return (long)i;
    return -1;
}
