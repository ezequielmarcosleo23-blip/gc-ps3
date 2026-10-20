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
 * printf de un programa emulado: el formato y las cadenas están en la RAM de
 * la GameCube y los argumentos, donde los deja el ABI SVR4/EABI del PowerPC:
 *
 *   enteros de 32 bits   r3-r10, después la pila (de a 4 bytes)
 *   long long            un par impar-par (r3:r4, r5:r6...), después la pila
 *                        alineada a 8; si no entra, ya no se usan más GPR
 *   double (y float)     f1-f8, después la pila alineada a 8
 *
 * La pila de desborde empieza en r1 + 8 (el área de parámetros del que
 * llama). Con va_list (vprintf), los registros salen del área que guardó
 * va_start: {gpr, fpr, reservado, overflow_arg_area, reg_save_area}.
 * Mismo criterio que HLE_VarArgs de Dolphin.
 */
#ifndef GCPS3_CORE_VAFORMAT_H
#define GCPS3_CORE_VAFORMAT_H

#include <stddef.h>
#include <stdint.h>

#include "core/mem.h"
#include "cpu/gekko.h"

typedef struct gc_va {
    gc_mem_t      *mem;
    const gekko_t *cpu;
    uint32_t       gpr;           /* próximo GPR (3-10; 11 = agotados) */
    uint32_t       fpr;           /* próximo FPR (1-8; 9 = agotados) */
    uint32_t       stack;         /* próximo argumento en la pila */
    uint32_t       gpr_area;      /* va_list: dónde guardó va_start r3-r10 (0 = registros) */
    uint32_t       fpr_area;
} gc_va_t;

/* Argumentos variables desde los registros: el primero en r`first_gpr`. */
void gc_va_from_regs(gc_va_t *va, gc_mem_t *mem, const gekko_t *c, uint32_t first_gpr);

/* Argumentos desde una va_list que está en la RAM, en `addr`. */
void gc_va_from_list(gc_va_t *va, gc_mem_t *mem, const gekko_t *c, uint32_t addr);

uint32_t gc_va_u32(gc_va_t *va);
uint64_t gc_va_u64(gc_va_t *va);
double   gc_va_double(gc_va_t *va);

/* Copia hasta cap-1 bytes de la cadena en `addr` (termina en 0). Devuelve el largo. */
size_t gc_read_cstr(gc_mem_t *mem, uint32_t addr, char *out, size_t cap);

/* Formatea como printf con el formato en `fmt_addr`. Devuelve el largo escrito
 * (cortado a cap-1). Sin %n: no escribe en la memoria emulada. */
size_t gc_vaformat(gc_mem_t *mem, uint32_t fmt_addr, gc_va_t *va, char *out, size_t cap);

#endif
