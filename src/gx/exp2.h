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
 * 2^x para la niebla del TEV. Dolphin llama a pow(2.0, x) de la libm del PC
 * (glibc), y cada libm redondea a su manera; esta versión usa solo suma,
 * resta, multiplicación y división de double, así que da lo mismo en el PC y
 * en el PPU. Calcula en double-double y redondea una sola vez: el resultado es
 * el double más cercano a 2^x.
 *
 * Dominio: x es un float en [-8, 0] (el -8 * f de la niebla) o NaN.
 * tools/exp2/comprobar.c lo compara con glibc para todos esos floats.
 */
#ifndef GCPS3_GX_EXP2_H
#define GCPS3_GX_EXP2_H

double gx_exp2(float x);

#endif
