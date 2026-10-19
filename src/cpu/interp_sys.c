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

/* Sistema: SPR, MSR, CR completo, segmentos, sc y rfi. Los juegos corren en
 * modo supervisor, así que no se comprueban privilegios. */
#include "cpu/interp.h"

static void op_mfspr(gekko_t *c, uint32_t i) { c->gpr[RD(i)] = gekko_get_spr(c, SPRN(i)); }
static void op_mtspr(gekko_t *c, uint32_t i) { gekko_set_spr(c, SPRN(i), c->gpr[RS(i)]); }
static void op_mftb(gekko_t *c, uint32_t i)  { c->gpr[RD(i)] = gekko_get_spr(c, SPRN(i)); }
static void op_mfmsr(gekko_t *c, uint32_t i) { c->gpr[RD(i)] = c->msr; }
static void op_mtmsr(gekko_t *c, uint32_t i) { c->msr = c->gpr[RS(i)]; }
static void op_mfcr(gekko_t *c, uint32_t i)  { c->gpr[RD(i)] = c->cr; }

static void op_mtcrf(gekko_t *c, uint32_t i)
{
    const uint32_t crm = (i >> 12) & 0xFFu;
    uint32_t mask = 0;
    for (uint32_t k = 0; k < 8; k++)
        if (crm & (0x80u >> k))
            mask |= 0xF0000000u >> (4 * k);
    c->cr = (c->gpr[RS(i)] & mask) | (c->cr & ~mask);
}

static void op_mcrxr(gekko_t *c, uint32_t i)
{
    cr_set(c, CRFD(i), c->xer >> 28);
    c->xer &= ~0xF0000000u;
}

static void op_mfsr(gekko_t *c, uint32_t i)   { c->gpr[RD(i)] = c->sr[(i >> 16) & 15u]; }
static void op_mtsr(gekko_t *c, uint32_t i)   { c->sr[(i >> 16) & 15u] = c->gpr[RS(i)]; }
static void op_mfsrin(gekko_t *c, uint32_t i) { c->gpr[RD(i)] = c->sr[c->gpr[RB(i)] >> 28]; }
static void op_mtsrin(gekko_t *c, uint32_t i) { c->sr[c->gpr[RB(i)] >> 28] = c->gpr[RS(i)]; }

static void op_sc(gekko_t *c, uint32_t i)
{
    (void)i;
    gekko_exception(c, EXC_SYSCALL, c->pc + 4, 0);
}

static void op_rfi(gekko_t *c, uint32_t i)
{
    (void)i;
    const uint32_t mask = 0x87C0FFFFu;
    c->msr = ((c->msr & ~mask) | (c->spr[SPR_SRR1] & mask)) & ~MSR_POW;
    c->npc = c->spr[SPR_SRR0] & ~3u;
    c->reserve = 0;
}

static void op_tlb_nop(gekko_t *c, uint32_t i)
{
    (void)c;
    (void)i;
}

void gekko_register_sys(void)
{
    gekko_op_primary[17] = op_sc;
    gekko_op_19[50] = op_rfi;
    gekko_op_fn *x = gekko_op_31;
    x[339] = op_mfspr; x[467] = op_mtspr; x[371] = op_mftb;
    x[83] = op_mfmsr;  x[146] = op_mtmsr; x[19] = op_mfcr;   x[144] = op_mtcrf;
    x[512] = op_mcrxr;
    x[595] = op_mfsr;  x[210] = op_mtsr;  x[659] = op_mfsrin; x[242] = op_mtsrin;
    x[306] = op_tlb_nop; x[566] = op_tlb_nop;
}
