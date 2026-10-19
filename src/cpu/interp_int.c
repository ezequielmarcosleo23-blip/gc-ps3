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
 * Enteros: aritmética, lógica, comparaciones, rotaciones y desplazamientos.
 * Verificado contra un PowerPC 750CXe emulado (tools/ppc-oracle) salvo los
 * casos que la arquitectura deja sin definir, que siguen a Dolphin.
 */
#include "cpu/interp.h"

/* a + b + cin con el acarreo de salida. */
static inline uint32_t add3(uint32_t a, uint32_t b, uint32_t cin, int *ca)
{
    const uint64_t s = (uint64_t)a + b + cin;
    *ca = (int)(s >> 32);
    return (uint32_t)s;
}

static inline int add_ov(uint32_t a, uint32_t b, uint32_t r)
{
    return (int)((((a ^ r) & (b ^ r)) >> 31) & 1u);
}

static inline uint32_t ca_in(const gekko_t *c)
{
    return (c->xer & XER_CA) ? 1u : 0u;
}

/* Termina una forma XO: OV si OE, CR0 si Rc. */
static inline void xo_finish(gekko_t *c, uint32_t inst, uint32_t r, int ov)
{
    if (OE_BIT(inst))
        xer_set_ov(c, ov);
    if (RC_BIT(inst))
        cr0_update(c, r);
}

/* ---- inmediatos ----------------------------------------------------------- */

static void op_addi(gekko_t *c, uint32_t i)
{
    c->gpr[RD(i)] = (RA(i) ? c->gpr[RA(i)] : 0) + (uint32_t)SIMM(i);
}

static void op_addis(gekko_t *c, uint32_t i)
{
    c->gpr[RD(i)] = (RA(i) ? c->gpr[RA(i)] : 0) + ((uint32_t)SIMM(i) << 16);
}

static void op_addic(gekko_t *c, uint32_t i)
{
    int ca;
    c->gpr[RD(i)] = add3(c->gpr[RA(i)], (uint32_t)SIMM(i), 0, &ca);
    xer_set_ca(c, ca);
}

static void op_addic_rc(gekko_t *c, uint32_t i)
{
    op_addic(c, i);
    cr0_update(c, c->gpr[RD(i)]);
}

static void op_subfic(gekko_t *c, uint32_t i)
{
    int ca;
    c->gpr[RD(i)] = add3(~c->gpr[RA(i)], (uint32_t)SIMM(i), 1, &ca);
    xer_set_ca(c, ca);
}

static void op_mulli(gekko_t *c, uint32_t i)
{
    c->gpr[RD(i)] = c->gpr[RA(i)] * (uint32_t)SIMM(i);      /* los 32 bits bajos no dependen del signo */
}

static uint32_t cmp_signed(const gekko_t *c, int32_t a, int32_t b)
{
    return (a < b ? 8u : a > b ? 4u : 2u) | ((c->xer & XER_SO) ? 1u : 0u);
}

static uint32_t cmp_unsigned(const gekko_t *c, uint32_t a, uint32_t b)
{
    return (a < b ? 8u : a > b ? 4u : 2u) | ((c->xer & XER_SO) ? 1u : 0u);
}

static void op_cmpi(gekko_t *c, uint32_t i)
{
    cr_set(c, CRFD(i), cmp_signed(c, (int32_t)c->gpr[RA(i)], SIMM(i)));
}

static void op_cmpli(gekko_t *c, uint32_t i)
{
    cr_set(c, CRFD(i), cmp_unsigned(c, c->gpr[RA(i)], UIMM(i)));
}

static void op_ori(gekko_t *c, uint32_t i)   { c->gpr[RA(i)] = c->gpr[RS(i)] | UIMM(i); }
static void op_oris(gekko_t *c, uint32_t i)  { c->gpr[RA(i)] = c->gpr[RS(i)] | (UIMM(i) << 16); }
static void op_xori(gekko_t *c, uint32_t i)  { c->gpr[RA(i)] = c->gpr[RS(i)] ^ UIMM(i); }
static void op_xoris(gekko_t *c, uint32_t i) { c->gpr[RA(i)] = c->gpr[RS(i)] ^ (UIMM(i) << 16); }

static void op_andi_rc(gekko_t *c, uint32_t i)
{
    c->gpr[RA(i)] = c->gpr[RS(i)] & UIMM(i);
    cr0_update(c, c->gpr[RA(i)]);
}

static void op_andis_rc(gekko_t *c, uint32_t i)
{
    c->gpr[RA(i)] = c->gpr[RS(i)] & (UIMM(i) << 16);
    cr0_update(c, c->gpr[RA(i)]);
}

/* ---- rotaciones -------------------------------------------------------------- */

static inline uint32_t rotl(uint32_t v, uint32_t n)
{
    n &= 31;
    return n ? (v << n) | (v >> (32 - n)) : v;
}

/* Máscara de mb a me (bits de IBM), con vuelta si mb > me. */
static inline uint32_t mask32(uint32_t mb, uint32_t me)
{
    const uint32_t begin = 0xFFFFFFFFu >> mb;
    const uint32_t end = me < 31 ? (0xFFFFFFFFu >> (me + 1)) : 0;
    const uint32_t m = begin ^ end;
    return me < mb ? ~m : m;
}

static void op_rlwimi(gekko_t *c, uint32_t i)
{
    const uint32_t m = mask32(MB(i), ME(i));
    c->gpr[RA(i)] = (rotl(c->gpr[RS(i)], SH(i)) & m) | (c->gpr[RA(i)] & ~m);
    if (RC_BIT(i))
        cr0_update(c, c->gpr[RA(i)]);
}

static void op_rlwinm(gekko_t *c, uint32_t i)
{
    c->gpr[RA(i)] = rotl(c->gpr[RS(i)], SH(i)) & mask32(MB(i), ME(i));
    if (RC_BIT(i))
        cr0_update(c, c->gpr[RA(i)]);
}

static void op_rlwnm(gekko_t *c, uint32_t i)
{
    c->gpr[RA(i)] = rotl(c->gpr[RS(i)], c->gpr[RB(i)] & 31) & mask32(MB(i), ME(i));
    if (RC_BIT(i))
        cr0_update(c, c->gpr[RA(i)]);
}

/* ---- trap -------------------------------------------------------------------- */

static int trap_cond(uint32_t to, uint32_t a, uint32_t b)
{
    const int32_t sa = (int32_t)a, sb = (int32_t)b;
    return ((to & 0x10) && sa < sb) || ((to & 0x08) && sa > sb) || ((to & 0x04) && a == b) ||
           ((to & 0x02) && a < b) || ((to & 0x01) && a > b);
}

static void op_twi(gekko_t *c, uint32_t i)
{
    if (trap_cond(RD(i), c->gpr[RA(i)], (uint32_t)SIMM(i)))
        gekko_exception(c, EXC_PROGRAM, c->pc, SRR1_PROGRAM_TRAP);
}

static void op_tw(gekko_t *c, uint32_t i)
{
    if (trap_cond(RD(i), c->gpr[RA(i)], c->gpr[RB(i)]))
        gekko_exception(c, EXC_PROGRAM, c->pc, SRR1_PROGRAM_TRAP);
}

/* ---- opcode 31: comparaciones ------------------------------------------------ */

static void op_cmp(gekko_t *c, uint32_t i)
{
    cr_set(c, CRFD(i), cmp_signed(c, (int32_t)c->gpr[RA(i)], (int32_t)c->gpr[RB(i)]));
}

static void op_cmpl(gekko_t *c, uint32_t i)
{
    cr_set(c, CRFD(i), cmp_unsigned(c, c->gpr[RA(i)], c->gpr[RB(i)]));
}

/* ---- opcode 31: suma y resta (forma XO) --------------------------------------- */

static void op_add(gekko_t *c, uint32_t i)
{
    const uint32_t a = c->gpr[RA(i)], b = c->gpr[RB(i)], r = a + b;
    c->gpr[RD(i)] = r;
    xo_finish(c, i, r, add_ov(a, b, r));
}

static void op_addc(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = c->gpr[RA(i)], b = c->gpr[RB(i)], r = add3(a, b, 0, &ca);
    c->gpr[RD(i)] = r;
    xer_set_ca(c, ca);
    xo_finish(c, i, r, add_ov(a, b, r));
}

static void op_adde(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = c->gpr[RA(i)], b = c->gpr[RB(i)], r = add3(a, b, ca_in(c), &ca);
    c->gpr[RD(i)] = r;
    xer_set_ca(c, ca);
    xo_finish(c, i, r, add_ov(a, b, r));
}

static void op_addme(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = c->gpr[RA(i)], r = add3(a, 0xFFFFFFFFu, ca_in(c), &ca);
    c->gpr[RD(i)] = r;
    xer_set_ca(c, ca);
    xo_finish(c, i, r, add_ov(a, 0xFFFFFFFFu, r));
}

static void op_addze(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = c->gpr[RA(i)], r = add3(a, 0, ca_in(c), &ca);
    c->gpr[RD(i)] = r;
    xer_set_ca(c, ca);
    xo_finish(c, i, r, add_ov(a, 0, r));
}

static void op_subf(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = ~c->gpr[RA(i)], b = c->gpr[RB(i)], r = add3(a, b, 1, &ca);
    c->gpr[RD(i)] = r;
    xo_finish(c, i, r, add_ov(a, b, r));
}

static void op_subfc(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = ~c->gpr[RA(i)], b = c->gpr[RB(i)], r = add3(a, b, 1, &ca);
    c->gpr[RD(i)] = r;
    xer_set_ca(c, ca);
    xo_finish(c, i, r, add_ov(a, b, r));
}

static void op_subfe(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = ~c->gpr[RA(i)], b = c->gpr[RB(i)], r = add3(a, b, ca_in(c), &ca);
    c->gpr[RD(i)] = r;
    xer_set_ca(c, ca);
    xo_finish(c, i, r, add_ov(a, b, r));
}

static void op_subfme(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = ~c->gpr[RA(i)], r = add3(a, 0xFFFFFFFFu, ca_in(c), &ca);
    c->gpr[RD(i)] = r;
    xer_set_ca(c, ca);
    xo_finish(c, i, r, add_ov(a, 0xFFFFFFFFu, r));
}

static void op_subfze(gekko_t *c, uint32_t i)
{
    int ca;
    const uint32_t a = ~c->gpr[RA(i)], r = add3(a, 0, ca_in(c), &ca);
    c->gpr[RD(i)] = r;
    xer_set_ca(c, ca);
    xo_finish(c, i, r, add_ov(a, 0, r));
}

static void op_neg(gekko_t *c, uint32_t i)
{
    const uint32_t a = c->gpr[RA(i)], r = 0u - a;
    c->gpr[RD(i)] = r;
    xo_finish(c, i, r, a == 0x80000000u);
}

/* ---- opcode 31: multiplicación y división ------------------------------------ */

static void op_mullw(gekko_t *c, uint32_t i)
{
    const int64_t p = (int64_t)(int32_t)c->gpr[RA(i)] * (int32_t)c->gpr[RB(i)];
    const uint32_t r = (uint32_t)p;
    c->gpr[RD(i)] = r;
    xo_finish(c, i, r, p != (int64_t)(int32_t)r);
}

static void op_mulhw(gekko_t *c, uint32_t i)
{
    const int64_t p = (int64_t)(int32_t)c->gpr[RA(i)] * (int32_t)c->gpr[RB(i)];
    c->gpr[RD(i)] = (uint32_t)((uint64_t)p >> 32);
    if (RC_BIT(i))
        cr0_update(c, c->gpr[RD(i)]);
}

static void op_mulhwu(gekko_t *c, uint32_t i)
{
    const uint64_t p = (uint64_t)c->gpr[RA(i)] * c->gpr[RB(i)];
    c->gpr[RD(i)] = (uint32_t)(p >> 32);
    if (RC_BIT(i))
        cr0_update(c, c->gpr[RD(i)]);
}

/* División por cero o 0x80000000 / -1: la arquitectura no lo define; Gekko da
 * 0xFFFFFFFF si el dividendo es negativo y 0 si no (como Dolphin). */
static void op_divw(gekko_t *c, uint32_t i)
{
    const int32_t a = (int32_t)c->gpr[RA(i)], b = (int32_t)c->gpr[RB(i)];
    const int ov = b == 0 || ((uint32_t)a == 0x80000000u && b == -1);
    const uint32_t r = ov ? (a < 0 ? 0xFFFFFFFFu : 0u) : (uint32_t)(a / b);
    c->gpr[RD(i)] = r;
    xo_finish(c, i, r, ov);
}

static void op_divwu(gekko_t *c, uint32_t i)
{
    const uint32_t a = c->gpr[RA(i)], b = c->gpr[RB(i)];
    const uint32_t r = b ? a / b : 0;
    c->gpr[RD(i)] = r;
    xo_finish(c, i, r, b == 0);
}

/* ---- opcode 31: lógica, desplazamientos, extensiones ------------------------- */

#define LOGIC(name, expr)                                                      \
    static void name(gekko_t *c, uint32_t i)                                   \
    {                                                                          \
        const uint32_t s = c->gpr[RS(i)], b = c->gpr[RB(i)];                   \
        (void)b;                                                               \
        c->gpr[RA(i)] = (expr);                                                \
        if (RC_BIT(i))                                                         \
            cr0_update(c, c->gpr[RA(i)]);                                      \
    }

LOGIC(op_and,  s & b)
LOGIC(op_andc, s & ~b)
LOGIC(op_or,   s | b)
LOGIC(op_orc,  s | ~b)
LOGIC(op_xor,  s ^ b)
LOGIC(op_nand, ~(s & b))
LOGIC(op_nor,  ~(s | b))
LOGIC(op_eqv,  ~(s ^ b))
LOGIC(op_slw,  (b & 0x20) ? 0u : s << (b & 31))
LOGIC(op_srw,  (b & 0x20) ? 0u : s >> (b & 31))
LOGIC(op_extsb, (uint32_t)(int32_t)(int8_t)s)
LOGIC(op_extsh, (uint32_t)(int32_t)(int16_t)s)
LOGIC(op_cntlzw, s ? (uint32_t)__builtin_clz(s) : 32u)

static void sraw_common(gekko_t *c, uint32_t i, uint32_t n)
{
    const int32_t s = (int32_t)c->gpr[RS(i)];
    uint32_t r;
    int ca;
    if (n > 31) {
        r = (uint32_t)(s >> 31);
        ca = s < 0;
    } else {
        r = (uint32_t)(s >> n);
        ca = s < 0 && n && ((uint32_t)s & ((1u << n) - 1));
    }
    c->gpr[RA(i)] = r;
    xer_set_ca(c, ca);
    if (RC_BIT(i))
        cr0_update(c, r);
}

static void op_sraw(gekko_t *c, uint32_t i)  { sraw_common(c, i, c->gpr[RB(i)] & 0x3F); }
static void op_srawi(gekko_t *c, uint32_t i) { sraw_common(c, i, SH(i)); }

void gekko_register_int(void)
{
    gekko_op_fn *p = gekko_op_primary;
    p[3] = op_twi;      p[7] = op_mulli;   p[8] = op_subfic;  p[10] = op_cmpli;
    p[11] = op_cmpi;    p[12] = op_addic;  p[13] = op_addic_rc; p[14] = op_addi;
    p[15] = op_addis;   p[20] = op_rlwimi; p[21] = op_rlwinm; p[23] = op_rlwnm;
    p[24] = op_ori;     p[25] = op_oris;   p[26] = op_xori;   p[27] = op_xoris;
    p[28] = op_andi_rc; p[29] = op_andis_rc;

    gekko_op_fn *x = gekko_op_31;
    x[0] = op_cmp;    x[32] = op_cmpl;   x[4] = op_tw;
    x[11] = op_mulhwu; x[75] = op_mulhw;
    reg31_oe(8, op_subfc);   reg31_oe(10, op_addc);   reg31_oe(40, op_subf);
    reg31_oe(104, op_neg);   reg31_oe(136, op_subfe); reg31_oe(138, op_adde);
    reg31_oe(200, op_subfze); reg31_oe(202, op_addze); reg31_oe(232, op_subfme);
    reg31_oe(234, op_addme); reg31_oe(235, op_mullw); reg31_oe(266, op_add);
    reg31_oe(459, op_divwu); reg31_oe(491, op_divw);
    x[28] = op_and;   x[60] = op_andc;  x[444] = op_or;   x[412] = op_orc;
    x[316] = op_xor;  x[476] = op_nand; x[124] = op_nor;  x[284] = op_eqv;
    x[24] = op_slw;   x[536] = op_srw;  x[792] = op_sraw; x[824] = op_srawi;
    x[954] = op_extsb; x[922] = op_extsh; x[26] = op_cntlzw;
}
