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

/* CRC-32 de zlib y PNG (polinomio 0xEDB88320). */
#ifndef GCPS3_CORE_CRC32_H
#define GCPS3_CORE_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Sigue un CRC: empezar con crc = 0. gc_crc32(0, "123456789", 9) = 0xCBF43926. */
uint32_t gc_crc32(uint32_t crc, const void *data, size_t len);

#endif
