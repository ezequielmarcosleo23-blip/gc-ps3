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
 * Interno del intérprete: campos de la instrucción, tablas de decodificación y
 * ayudas para CR y XER. Solo lo incluyen los archivos de src/cpu/.
 */
#ifndef GCPS3_CPU_INTERP_H
#define GCPS3_CPU_INTERP_H

#include <stdint.h>

#include "cpu/gekko.h"

typedef void (*gekko_op_fn)(gekko_t *c, uint32_t inst);

/* Campos (formato de los manuales: bit 0 = el más significativo). */
#define OPCD(i)   ((i) >> 26)
#define RD(i)     (((i) >> 21) & 31u)
#define RS(i)     RD(i)
#define RA(i)     (((i) >> 16) & 31u)
#define RB(i)     (((i) >> 11) & 31u)
#define RC(i)     (((i) >> 6) & 31u)
#define RC_BIT(i) ((i) & 1u)
#define OE_BIT(i) (((i) >> 10) & 1u)
#define SIMM(i)   ((int32_t)(int16_t)((i) & 0xFFFFu))
#define UIMM(i)   ((i) & 0xFFFFu)
#define XO10(i)   (((i) >> 1) & 0x3FFu)
#define XO5(i)    (((i) >> 1) & 0x1Fu)
#define SH(i)     RB(i)
#define MB(i)     (((i) >> 6) & 31u)
#define ME(i)     (((i) >> 1) & 31u)
#define CRFD(i)   (((i) >> 23) & 7u)
#define CRFS(i)   (((i) >> 18) & 7u)
#define BO(i)     RD(i)
#define BI(i)     RA(i)
#define LK(i)     ((i) & 1u)
#define AA(i)     (((i) >> 1) & 1u)
#define BD(i)     ((int32_t)(int16_t)((i) & 0xFFFCu))
#define LI(i)     (((int32_t)((i) << 6) >> 6) & ~3)
#define SPRN(i)   ((((i) >> 16) & 31u) | (((i) >> 6) & 0x3E0u))

/* Tablas: primaria y extendidas de los opcodes 4, 19, 31, 59 y 63. */
extern gekko_op_fn gekko_op_primary[64];
extern gekko_op_fn gekko_op_4[1024];
extern gekko_op_fn gekko_op_19[1024];
extern gekko_op_fn gekko_op_31[1024];
extern gekko_op_fn gekko_op_59[32];
extern gekko_op_fn gekko_op_63[1024];
extern gekko_op_fn gekko_op_63a[32];

void gekko_op_illegal(gekko_t *c, uint32_t inst);

/* Registro de instrucciones de cada archivo. */
void gekko_register_int(void);
void gekko_register_branch(void);
void gekko_register_mem(void);
void gekko_register_sys(void);
void gekko_register_fpu(void);
void gekko_register_ps(void);

/* MSR[FP] = 0: excepción de FPU no disponible (SRR0 = esta instrucción). */
static inline int gekko_fp_unavailable(gekko_t *c)
{
    if (c->msr & MSR_FP)
        return 0;
    gekko_exception(c, EXC_FP_UNAVAILABLE, c->pc, 0);
    return 1;
}

/* Registra una forma XO con y sin OE (xo y xo | 0x200). */
static inline void reg31_oe(uint32_t xo, gekko_op_fn fn)
{
    gekko_op_31[xo] = fn;
    gekko_op_31[xo | 0x200] = fn;
}

/* ---- CR ---------------------------------------------------------------- */

static inline uint32_t cr_get(const gekko_t *c, uint32_t field)
{
    return (c->cr >> (28 - 4 * field)) & 0xFu;
}

static inline void cr_set(gekko_t *c, uint32_t field, uint32_t v)
{
    const uint32_t shift = 28 - 4 * field;
    c->cr = (c->cr & ~(0xFu << shift)) | ((v & 0xFu) << shift);
}

static inline uint32_t cr_bit(const gekko_t *c, uint32_t bit)
{
    return (c->cr >> (31 - bit)) & 1u;
}

static inline void cr_set_bit(gekko_t *c, uint32_t bit, uint32_t v)
{
    const uint32_t m = 1u << (31 - bit);
    c->cr = v ? (c->cr | m) : (c->cr & ~m);
}

/* CR0 con LT/GT/EQ del resultado con signo y SO copiado de XER. */
static inline void cr0_update(gekko_t *c, uint32_t result)
{
    uint32_t v = (int32_t)result < 0 ? 0x8 : (result ? 0x4 : 0x2);
    cr_set(c, 0, v | ((c->xer & XER_SO) ? 1 : 0));
}

/* ---- XER --------------------------------------------------------------- */

static inline void xer_set_ca(gekko_t *c, int ca)
{
    c->xer = ca ? (c->xer | XER_CA) : (c->xer & ~XER_CA);
}

/* OV según `ov`; SO se queda en 1 una vez puesto. */
static inline void xer_set_ov(gekko_t *c, int ov)
{
    if (ov)
        c->xer |= XER_OV | XER_SO;
    else
        c->xer &= ~XER_OV;
}

/* Dirección efectiva de las formas D y X: (rA|0) + desplazamiento o rB. */
static inline uint32_t ea_d(const gekko_t *c, uint32_t inst)
{
    return (RA(inst) ? c->gpr[RA(inst)] : 0) + (uint32_t)SIMM(inst);
}

static inline uint32_t ea_x(const gekko_t *c, uint32_t inst)
{
    return (RA(inst) ? c->gpr[RA(inst)] : 0) + c->gpr[RB(inst)];
}

#endif
