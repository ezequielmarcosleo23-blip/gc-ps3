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
 * Las conversiones de float a entero como las compila GCC para x86-64, que es
 * donde corre la referencia (Dolphin). En C un float fuera de rango o NaN es
 * comportamiento indefinido, y el PPU satura distinto: acá queda escrito.
 */
#ifndef GCPS3_GX_X86_H
#define GCPS3_GX_X86_H

#include <stdint.h>

/* (s32)f, y también (u16) y (u8) con los bits de abajo: cvttss2si de 32 bits;
 * NaN o fuera de rango da INT32_MIN */
static inline int32_t gx_x86_f2i(float f)
{
    if (!(f >= -2147483648.0f && f < 2147483648.0f))
        return INT32_MIN;
    return (int32_t)f;
}

/* (u32)f: cvttss2si de 64 bits y los 32 de abajo; NaN o fuera de rango da 0 */
static inline uint32_t gx_x86_f2u(float f)
{
    if (!(f > -9223372036854775808.0f && f < 9223372036854775808.0f))
        return 0;
    return (uint32_t)(uint64_t)(int64_t)f;
}

/* (u32)d de un double, igual */
static inline uint32_t gx_x86_d2u(double d)
{
    if (!(d > -9223372036854775808.0 && d < 9223372036854775808.0))
        return 0;
    return (uint32_t)(uint64_t)(int64_t)d;
}

#endif
