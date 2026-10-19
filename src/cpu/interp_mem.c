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
 * Lecturas y escrituras de enteros, múltiples, de cadenas, con bytes
 * invertidos, reservas (lwarx/stwcx.) y operaciones de caché. Todo pasa por
 * core/mem.c, que resuelve RAM, registros de hardware y caché bloqueada.
 */
#include "cpu/interp.h"

static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }

/* ---- forma D ----------------------------------------------------------------- */

#define LOAD_D(name, expr)                                                     \
    static void name(gekko_t *c, uint32_t i)                                   \
    {                                                                          \
        const uint32_t ea = ea_d(c, i);                                        \
        c->gpr[RD(i)] = (expr);                                                \
    }                                                                          \
    static void name##u(gekko_t *c, uint32_t i)                                \
    {                                                                          \
        const uint32_t ea = c->gpr[RA(i)] + (uint32_t)SIMM(i);                 \
        c->gpr[RD(i)] = (expr);                                                \
        c->gpr[RA(i)] = ea;                                                    \
    }

LOAD_D(op_lwz, gc_read32(c->mem, ea))
LOAD_D(op_lbz, gc_read8(c->mem, ea))
LOAD_D(op_lhz, gc_read16(c->mem, ea))
LOAD_D(op_lha, (uint32_t)(int32_t)(int16_t)gc_read16(c->mem, ea))

#define STORE_D(name, stmt)                                                    \
    static void name(gekko_t *c, uint32_t i)                                   \
    {                                                                          \
        const uint32_t ea = ea_d(c, i);                                        \
        const uint32_t s = c->gpr[RS(i)];                                      \
        stmt;                                                                  \
    }                                                                          \
    static void name##u(gekko_t *c, uint32_t i)                                \
    {                                                                          \
        const uint32_t ea = c->gpr[RA(i)] + (uint32_t)SIMM(i);                 \
        const uint32_t s = c->gpr[RS(i)];                                      \
        stmt;                                                                  \
        c->gpr[RA(i)] = ea;                                                    \
    }

STORE_D(op_stw, gc_write32(c->mem, ea, s))
STORE_D(op_stb, gc_write8(c->mem, ea, (uint8_t)s))
STORE_D(op_sth, gc_write16(c->mem, ea, (uint16_t)s))

static void op_lmw(gekko_t *c, uint32_t i)
{
    uint32_t ea = ea_d(c, i);
    for (uint32_t r = RD(i); r < 32; r++, ea += 4)
        c->gpr[r] = gc_read32(c->mem, ea);
}

static void op_stmw(gekko_t *c, uint32_t i)
{
    uint32_t ea = ea_d(c, i);
    for (uint32_t r = RS(i); r < 32; r++, ea += 4)
        gc_write32(c->mem, ea, c->gpr[r]);
}

/* ---- forma X ----------------------------------------------------------------- */

#define LOAD_X(name, expr)                                                     \
    static void name(gekko_t *c, uint32_t i)                                   \
    {                                                                          \
        const uint32_t ea = ea_x(c, i);                                        \
        c->gpr[RD(i)] = (expr);                                                \
    }                                                                          \
    static void name##u(gekko_t *c, uint32_t i)                                \
    {                                                                          \
        const uint32_t ea = c->gpr[RA(i)] + c->gpr[RB(i)];                     \
        c->gpr[RD(i)] = (expr);                                                \
        c->gpr[RA(i)] = ea;                                                    \
    }

LOAD_X(op_lwzx, gc_read32(c->mem, ea))
LOAD_X(op_lbzx, gc_read8(c->mem, ea))
LOAD_X(op_lhzx, gc_read16(c->mem, ea))
LOAD_X(op_lhax, (uint32_t)(int32_t)(int16_t)gc_read16(c->mem, ea))

#define STORE_X(name, stmt)                                                    \
    static void name(gekko_t *c, uint32_t i)                                   \
    {                                                                          \
        const uint32_t ea = ea_x(c, i);                                        \
        const uint32_t s = c->gpr[RS(i)];                                      \
        stmt;                                                                  \
    }                                                                          \
    static void name##u(gekko_t *c, uint32_t i)                                \
    {                                                                          \
        const uint32_t ea = c->gpr[RA(i)] + c->gpr[RB(i)];                     \
        const uint32_t s = c->gpr[RS(i)];                                      \
        stmt;                                                                  \
        c->gpr[RA(i)] = ea;                                                    \
    }

STORE_X(op_stwx, gc_write32(c->mem, ea, s))
STORE_X(op_stbx, gc_write8(c->mem, ea, (uint8_t)s))
STORE_X(op_sthx, gc_write16(c->mem, ea, (uint16_t)s))

static void op_lwbrx(gekko_t *c, uint32_t i)  { c->gpr[RD(i)] = bswap32(gc_read32(c->mem, ea_x(c, i))); }
static void op_lhbrx(gekko_t *c, uint32_t i)  { c->gpr[RD(i)] = bswap16(gc_read16(c->mem, ea_x(c, i))); }
static void op_stwbrx(gekko_t *c, uint32_t i) { gc_write32(c->mem, ea_x(c, i), bswap32(c->gpr[RS(i)])); }
static void op_sthbrx(gekko_t *c, uint32_t i) { gc_write16(c->mem, ea_x(c, i), bswap16((uint16_t)c->gpr[RS(i)])); }

/* ---- reservas ------------------------------------------------------------------ */

static void op_lwarx(gekko_t *c, uint32_t i)
{
    const uint32_t ea = ea_x(c, i);
    c->gpr[RD(i)] = gc_read32(c->mem, ea);
    c->reserve = 1;
    c->reserve_addr = ea;
}

static void op_stwcx_rc(gekko_t *c, uint32_t i)
{
    const uint32_t ea = ea_x(c, i);
    uint32_t cr0 = (c->xer & XER_SO) ? 1u : 0u;
    if (c->reserve && c->reserve_addr == ea) {
        gc_write32(c->mem, ea, c->gpr[RS(i)]);
        cr0 |= 2u;
    }
    c->reserve = 0;
    cr_set(c, 0, cr0);
}

/* ---- cadenas: bytes en registros consecutivos, de izquierda a derecha ---------- */

static void load_string(gekko_t *c, uint32_t rd, uint32_t ea, uint32_t n)
{
    uint32_t r = (rd + 31) & 31, shift = 0;
    for (; n; n--, ea++) {
        if (shift == 0) {
            r = (r + 1) & 31;
            c->gpr[r] = 0;
        }
        c->gpr[r] |= (uint32_t)gc_read8(c->mem, ea) << (24 - shift);
        shift = (shift + 8) & 31;
    }
}

static void store_string(gekko_t *c, uint32_t rs, uint32_t ea, uint32_t n)
{
    uint32_t r = (rs + 31) & 31, shift = 0;
    for (; n; n--, ea++) {
        if (shift == 0)
            r = (r + 1) & 31;
        gc_write8(c->mem, ea, (uint8_t)(c->gpr[r] >> (24 - shift)));
        shift = (shift + 8) & 31;
    }
}

static void op_lswi(gekko_t *c, uint32_t i)
{
    load_string(c, RD(i), RA(i) ? c->gpr[RA(i)] : 0, RB(i) ? RB(i) : 32);
}

static void op_lswx(gekko_t *c, uint32_t i)
{
    load_string(c, RD(i), ea_x(c, i), c->xer & XER_BC);
}

static void op_stswi(gekko_t *c, uint32_t i)
{
    store_string(c, RS(i), RA(i) ? c->gpr[RA(i)] : 0, RB(i) ? RB(i) : 32);
}

static void op_stswx(gekko_t *c, uint32_t i)
{
    store_string(c, RS(i), ea_x(c, i), c->xer & XER_BC);
}

/* ---- caché ---------------------------------------------------------------------- */

static void zero_line(gekko_t *c, uint32_t ea)
{
    ea &= ~31u;
    for (uint32_t k = 0; k < 32; k += 4)
        gc_write32(c->mem, ea + k, 0);
}

static void op_dcbz(gekko_t *c, uint32_t i)   { zero_line(c, ea_x(c, i)); }

/* dcbz_l (opcode 4): pone a cero una línea de la caché bloqueada (0xE0000000). */
static void op_dcbz_l(gekko_t *c, uint32_t i) { zero_line(c, ea_x(c, i)); }

/* El intérprete no modela cachés: dcbf, dcbst, dcbi, dcbt, dcbtst, icbi, sync,
 * eieio no hacen nada. El JIT engancha icbi para invalidar bloques traducidos. */
static void op_nop(gekko_t *c, uint32_t i)
{
    (void)c;
    (void)i;
}

void gekko_register_mem(void)
{
    gekko_op_fn *p = gekko_op_primary;
    p[32] = op_lwz;  p[33] = op_lwzu;  p[34] = op_lbz;  p[35] = op_lbzu;
    p[40] = op_lhz;  p[41] = op_lhzu;  p[42] = op_lha;  p[43] = op_lhau;
    p[36] = op_stw;  p[37] = op_stwu;  p[38] = op_stb;  p[39] = op_stbu;
    p[44] = op_sth;  p[45] = op_sthu;  p[46] = op_lmw;  p[47] = op_stmw;

    gekko_op_fn *x = gekko_op_31;
    x[23] = op_lwzx;   x[55] = op_lwzxu;  x[87] = op_lbzx;   x[119] = op_lbzxu;
    x[279] = op_lhzx;  x[311] = op_lhzxu; x[343] = op_lhax;  x[375] = op_lhaxu;
    x[151] = op_stwx;  x[183] = op_stwxu; x[215] = op_stbx;  x[247] = op_stbxu;
    x[407] = op_sthx;  x[439] = op_sthxu;
    x[534] = op_lwbrx; x[790] = op_lhbrx; x[662] = op_stwbrx; x[918] = op_sthbrx;
    x[20] = op_lwarx;  x[150] = op_stwcx_rc;
    x[597] = op_lswi;  x[533] = op_lswx;  x[725] = op_stswi; x[661] = op_stswx;
    x[1014] = op_dcbz;
    x[86] = op_nop;  x[54] = op_nop;  x[470] = op_nop;  x[278] = op_nop;
    x[246] = op_nop; x[982] = op_nop; x[598] = op_nop;  x[854] = op_nop;
    gekko_op_4[1014] = op_dcbz_l;
}
