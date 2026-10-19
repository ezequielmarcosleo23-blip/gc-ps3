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

/* Saltos y operaciones sobre CR. */
#include "cpu/interp.h"

static void op_b(gekko_t *c, uint32_t i)
{
    const uint32_t target = (uint32_t)LI(i) + (AA(i) ? 0 : c->pc);
    if (LK(i))
        c->lr = c->pc + 4;
    c->npc = target;
}

/* BO: 0x10 no mirar CR, 0x08 valor buscado, 0x04 no tocar CTR, 0x02 saltar si CTR == 0. */
static inline int bo_ctr_ok(gekko_t *c, uint32_t bo)
{
    if (bo & 0x04)
        return 1;
    c->ctr--;
    return (c->ctr != 0) != ((bo & 0x02) != 0);
}

static inline int bo_cond_ok(const gekko_t *c, uint32_t bo, uint32_t bi)
{
    return (bo & 0x10) || cr_bit(c, bi) == ((bo >> 3) & 1u);
}

static void op_bc(gekko_t *c, uint32_t i)
{
    const int ctr_ok = bo_ctr_ok(c, BO(i));
    const int cond_ok = bo_cond_ok(c, BO(i), BI(i));
    if (ctr_ok && cond_ok)
        c->npc = (uint32_t)BD(i) + (AA(i) ? 0 : c->pc);
    if (LK(i))
        c->lr = c->pc + 4;
}

static void op_bclr(gekko_t *c, uint32_t i)
{
    const uint32_t target = c->lr & ~3u;
    const int ctr_ok = bo_ctr_ok(c, BO(i));
    const int cond_ok = bo_cond_ok(c, BO(i), BI(i));
    if (ctr_ok && cond_ok)
        c->npc = target;
    if (LK(i))
        c->lr = c->pc + 4;
}

static void op_bcctr(gekko_t *c, uint32_t i)
{
    if (bo_cond_ok(c, BO(i), BI(i)))
        c->npc = c->ctr & ~3u;
    if (LK(i))
        c->lr = c->pc + 4;
}

#define CRLOGIC(name, expr)                                                    \
    static void name(gekko_t *c, uint32_t i)                                   \
    {                                                                          \
        const uint32_t a = cr_bit(c, RA(i)), b = cr_bit(c, RB(i));             \
        cr_set_bit(c, RD(i), (expr) & 1u);                                     \
    }

CRLOGIC(op_crand,  a & b)
CRLOGIC(op_crandc, a & ~b)
CRLOGIC(op_creqv,  ~(a ^ b))
CRLOGIC(op_crnand, ~(a & b))
CRLOGIC(op_crnor,  ~(a | b))
CRLOGIC(op_cror,   a | b)
CRLOGIC(op_crorc,  a | ~b)
CRLOGIC(op_crxor,  a ^ b)

static void op_mcrf(gekko_t *c, uint32_t i)
{
    cr_set(c, CRFD(i), cr_get(c, CRFS(i)));
}

static void op_isync(gekko_t *c, uint32_t i)
{
    (void)c;
    (void)i;
}

void gekko_register_branch(void)
{
    gekko_op_primary[16] = op_bc;
    gekko_op_primary[18] = op_b;
    gekko_op_fn *x = gekko_op_19;
    x[0] = op_mcrf;    x[16] = op_bclr;   x[528] = op_bcctr; x[150] = op_isync;
    x[257] = op_crand; x[129] = op_crandc; x[289] = op_creqv; x[225] = op_crnand;
    x[33] = op_crnor;  x[449] = op_cror;  x[417] = op_crorc; x[193] = op_crxor;
}
