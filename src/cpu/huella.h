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
 * Huella del estado de la CPU, para comparar instrucción por instrucción con
 * Dolphin (tools/dolphin-trace, docs/traza-dolphin.md). El parche de Dolphin
 * calcula exactamente lo mismo; si se cambia algo acá, se cambia allá y la
 * versión del formato.
 *
 * El estado son 89 palabras de 64 bits, en este orden:
 *   0-15   GPR de a pares: gpr[2k] | gpr[2k+1] << 32
 *   16-79  FPR: ps0 y ps1 de cada registro (f0.ps0, f0.ps1, f1.ps0, ...)
 *   80     CR | XER << 32
 *   81     LR | CTR << 32
 *   82     MSR | FPSCR << 32
 *   83     SRR0 | SRR1 << 32
 *   84-87  GQR de a pares
 *   88     HID0 | HID2 << 32
 * La huella es una ronda tipo xxHash por palabra, desde 0. La huella
 * encadenada de una traza pasa por la misma ronda (pc | inst << 32) y la
 * huella del estado de cada instrucción.
 */
#ifndef GCPS3_CPU_HUELLA_H
#define GCPS3_CPU_HUELLA_H

#include <stdint.h>

#include "cpu/gekko.h"

#define GEKKO_HUELLA_VERSION 1
#define GEKKO_ESTADO_PALABRAS 89

static inline uint64_t gekko_huella_ronda(uint64_t h, uint64_t w)
{
    h += w * 0xC2B2AE3D27D4EB4Full;
    h = (h << 31) | (h >> 33);
    return h * 0x9E3779B185EBCA87ull;
}

/* Las 89 palabras del estado */
void gekko_estado(const gekko_t *c, uint64_t w[GEKKO_ESTADO_PALABRAS]);

/* La huella del estado */
uint64_t gekko_huella(const gekko_t *c);

/* Suma una instrucción a la huella encadenada de una traza */
static inline uint64_t gekko_huella_cadena(uint64_t cadena, uint32_t pc, uint32_t inst,
                                           uint64_t estado)
{
    cadena = gekko_huella_ronda(cadena, (uint64_t)pc | (uint64_t)inst << 32);
    return gekko_huella_ronda(cadena, estado);
}

#endif
