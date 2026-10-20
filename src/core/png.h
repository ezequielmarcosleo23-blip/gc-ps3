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
 * PNG de un cuadro RGB sin librerías: 8 bits por canal, sin filtros y con
 * bloques "stored" de deflate (sin comprimir). Pesa un poco más que el cuadro
 * (unos 900 KB para 640x480), pero lo abre cualquier visor y el mismo código
 * sirve en la PC y en la PS3.
 */
#ifndef GCPS3_CORE_PNG_H
#define GCPS3_CORE_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Bytes que ocupa el PNG de un cuadro de w x h. */
size_t gc_png_size(uint32_t w, uint32_t h);

/* Escribe el PNG en out (cap bytes). Devuelve los bytes escritos, o 0 si no entra. */
size_t gc_png_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap);

#endif
