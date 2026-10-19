/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008 Dolphin Emulator Project
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
 * FPU escalar: aritmética (opcodes 59 y 63), conversiones, comparaciones,
 * FPSCR y cargas/almacenamientos de punto flotante.
 *
 * Semántica de Dolphin (commit cf5cb7ef), archivos Interpreter_FloatingPoint.cpp,
 * Interpreter_LoadStore.cpp e Interpreter_SystemRegisters.cpp:
 *   - las de precisión simple dejan el resultado en ps0 y ps1; las dobles, solo ps0
 *   - fmuls y fmadds redondean frC a 25 bits (fpu_force25bit)
 *   - fctiw deja 0xFFF80000 en la palabra alta, como el hardware
 *   - FI, FR, OX, UX y XX no se calculan siempre (tampoco en Dolphin)
 * MSR[FP] = 0 lo revisan gekko.c (opcodes 59 y 63) y las cargas de acá.
 */
#include <math.h>

#include "cpu/fpu_util.h"
#include "cpu/interp.h"

#define FD(i) RD(i)
#define FS(i) RS(i)
#define FA(i) RA(i)
#define FB(i) RB(i)
#define FC(i) RC(i)

static inline void rc1(gekko_t *c, uint32_t i)
{
    if (RC_BIT(i))
        fpu_cr1(c);
}

static inline double neg_unless_nan(double v)
{
    return fp_is_nan(v) ? v : f64_of(f64_bits(v) ^ DOUBLE_SIGN);
}

static inline float negf_unless_nan(float v)
{
    return f32_is_nan(v) ? v : f32_of(f32_bits(v) ^ FLOAT_SIGN);
}

/* Con FPSCR[VE] y una operación inválida, el destino no cambia. */
static inline int may_write(const gekko_t *c, fp_result_t r)
{
    return !(c->fpscr & FPSCR_VE) || fp_no_invalid(r);
}

static void put_double(gekko_t *c, uint32_t i, fp_result_t r, int negate)
{
    if (may_write(c, r)) {
        double v = fpu_force_double(c->fpscr, r.value);
        if (negate)
            v = neg_unless_nan(v);
        set_ps0(c, FD(i), v);
        fpu_fprf_double(c, v);
    }
    rc1(c, i);
}

static void put_single(gekko_t *c, uint32_t i, fp_result_t r, int negate)
{
    if (may_write(c, r)) {
        float v = fpu_force_single(c->fpscr, r.value);
        if (negate)
            v = negf_unless_nan(v);
        fill_single(c, FD(i), v);
        fpu_fprf_single(c, v);
    }
    rc1(c, i);
}

/* ---- aritmética --------------------------------------------------------------- */

static void op_fadd(gekko_t *c, uint32_t i)  { put_double(c, i, fpu_add(c, ps0(c, FA(i)), ps0(c, FB(i))), 0); }
static void op_fsub(gekko_t *c, uint32_t i)  { put_double(c, i, fpu_sub(c, ps0(c, FA(i)), ps0(c, FB(i))), 0); }
static void op_fadds(gekko_t *c, uint32_t i) { put_single(c, i, fpu_add(c, ps0(c, FA(i)), ps0(c, FB(i))), 0); }
static void op_fsubs(gekko_t *c, uint32_t i) { put_single(c, i, fpu_sub(c, ps0(c, FA(i)), ps0(c, FB(i))), 0); }

static void op_fmul(gekko_t *c, uint32_t i)
{
    const fp_result_t r = fpu_mul(c, ps0(c, FA(i)), ps0(c, FC(i)));
    if (may_write(c, r))
        fpu_clear_fifr(c);
    put_double(c, i, r, 0);
}

static void op_fmuls(gekko_t *c, uint32_t i)
{
    const fp_result_t r = fpu_mul(c, ps0(c, FA(i)), fpu_force25bit(ps0(c, FC(i))));
    if (may_write(c, r))
        fpu_clear_fifr(c);
    put_single(c, i, r, 0);
}

static void divide(gekko_t *c, uint32_t i, int single)
{
    const fp_result_t r = fpu_div(c, ps0(c, FA(i)), ps0(c, FB(i)));
    const int zero_ok = !(c->fpscr & FPSCR_ZE) || r.exception != FPSCR_ZX;
    if (!zero_ok || !may_write(c, r)) {
        rc1(c, i);
        return;
    }
    if (single)
        put_single(c, i, r, 0);
    else
        put_double(c, i, r, 0);
}

static void op_fdiv(gekko_t *c, uint32_t i)  { divide(c, i, 0); }
static void op_fdivs(gekko_t *c, uint32_t i) { divide(c, i, 1); }

/* fmadd y familia: frD = ±(frA * frC ± frB) */
#define MADD(name, sub, neg)                                                        \
    static void op_##name(gekko_t *c, uint32_t i)                                   \
    {                                                                               \
        put_double(c, i, fpu_madd(c, ps0(c, FA(i)), ps0(c, FC(i)), ps0(c, FB(i)), sub, 0), neg); \
    }                                                                               \
    static void op_##name##s(gekko_t *c, uint32_t i)                                \
    {                                                                               \
        const fp_result_t r = fpu_madd(c, ps0(c, FA(i)), ps0(c, FC(i)), ps0(c, FB(i)), sub, 1); \
        if (!(neg) && !(sub) && may_write(c, r)) {                                  \
            /* fmadds: FI = el resultado cambió al redondear; FR = 0 (Dolphin) */   \
            const float v = fpu_force_single(c->fpscr, r.value);                    \
            c->fpscr = (c->fpscr & ~(FPSCR_FI | FPSCR_FR)) |                        \
                       ((double)v != r.value ? FPSCR_FI : 0);                       \
        }                                                                           \
        put_single(c, i, r, neg);                                                   \
    }

MADD(fmadd, 0, 0)
MADD(fmsub, 1, 0)
MADD(fnmadd, 0, 1)
MADD(fnmsub, 1, 1)

static void op_fsel(gekko_t *c, uint32_t i)
{
    /* Binaria: no toca FPSCR. NaN en frA elige frB. */
    c->fpr[FD(i)][0] = ps0(c, FA(i)) >= -0.0 ? c->fpr[FC(i)][0] : c->fpr[FB(i)][0];
    rc1(c, i);
}

static void op_fres(gekko_t *c, uint32_t i)
{
    const double b = ps0(c, FB(i));
    int compute = 1;

    if (b == 0.0) {
        fpu_set_exception(c, FPSCR_ZX);
        fpu_clear_fifr(c);
        compute = !(c->fpscr & FPSCR_ZE);
    } else if (fp_is_snan(b)) {
        fpu_set_exception(c, FPSCR_VXSNAN);
        fpu_clear_fifr(c);
        compute = !(c->fpscr & FPSCR_VE);
    } else if (fp_is_nan(b) || fp_is_inf(b)) {
        fpu_clear_fifr(c);
    }
    if (compute) {
        const double r = fpu_approx_recip(b);
        c->fpr[FD(i)][0] = c->fpr[FD(i)][1] = f64_bits(r);
        fpu_fprf_single(c, (float)r);
    }
    rc1(c, i);
}

static void op_frsqrte(gekko_t *c, uint32_t i)
{
    const double b = ps0(c, FB(i));
    int compute = 1;

    if (b < 0.0) {
        fpu_set_exception(c, FPSCR_VXSQRT);
        fpu_clear_fifr(c);
        compute = !(c->fpscr & FPSCR_VE);
    } else if (b == 0.0) {
        fpu_set_exception(c, FPSCR_ZX);
        fpu_clear_fifr(c);
        compute = !(c->fpscr & FPSCR_ZE);
    } else if (fp_is_snan(b)) {
        fpu_set_exception(c, FPSCR_VXSNAN);
        fpu_clear_fifr(c);
        compute = !(c->fpscr & FPSCR_VE);
    } else if (fp_is_nan(b) || fp_is_inf(b)) {
        fpu_clear_fifr(c);
    }
    if (compute) {
        const double r = fpu_approx_rsqrt(b);
        set_ps0(c, FD(i), r);
        fpu_fprf_double(c, r);
    }
    rc1(c, i);
}

/* ---- movimientos (binarios: no tocan FPSCR) --------------------------------- */

static void op_fmr(gekko_t *c, uint32_t i)   { c->fpr[FD(i)][0] = c->fpr[FB(i)][0]; rc1(c, i); }
static void op_fneg(gekko_t *c, uint32_t i)  { c->fpr[FD(i)][0] = c->fpr[FB(i)][0] ^ DOUBLE_SIGN; rc1(c, i); }
static void op_fabs(gekko_t *c, uint32_t i)  { c->fpr[FD(i)][0] = c->fpr[FB(i)][0] & ~DOUBLE_SIGN; rc1(c, i); }
static void op_fnabs(gekko_t *c, uint32_t i) { c->fpr[FD(i)][0] = c->fpr[FB(i)][0] | DOUBLE_SIGN; rc1(c, i); }

/* ---- redondeo y conversión a entero ---------------------------------------- */

static void set_fi(gekko_t *c, int fi)
{
    if (fi)
        fpu_set_exception(c, FPSCR_XX);
    c->fpscr = fi ? (c->fpscr | FPSCR_FI) : (c->fpscr & ~FPSCR_FI);
}

static void op_frsp(gekko_t *c, uint32_t i)
{
    const double b = ps0(c, FB(i));
    const float rounded = fpu_force_single(c->fpscr, b);

    if (fp_is_nan(b)) {
        const int snan = fp_is_snan(b);
        if (snan)
            fpu_set_exception(c, FPSCR_VXSNAN);
        if (!snan || !(c->fpscr & FPSCR_VE)) {
            fill_single(c, FD(i), rounded);
            fpu_fprf_single(c, rounded);
        }
        fpu_clear_fifr(c);
    } else {
        set_fi(c, b != (double)rounded);
        c->fpscr = fabs((double)rounded) > fabs(b) ? (c->fpscr | FPSCR_FR) : (c->fpscr & ~FPSCR_FR);
        fpu_fprf_single(c, rounded);
        fill_single(c, FD(i), rounded);
    }
    rc1(c, i);
}

/* Redondea a entero con el modo de la máquina (= RN), sin cuidar NaN. */
static double round_to_integer_mode(double x)
{
    const double big = copysign(4503599627370496.0, x);     /* 2^52 */
    return (x + big) - big;
}

/* fctiw / fctiwz: conversión del apéndice C.4.2 del manual de entornos. */
static void convert_to_integer(gekko_t *c, uint32_t i, uint32_t rn)
{
    const double b = ps0(c, FB(i));
    double rounded;
    uint32_t value;
    int exception = 0;

    switch (rn) {
    case 0:  rounded = round_to_integer_mode(b); break;
    case 1:  rounded = trunc(b); break;
    case 2:  rounded = ceil(b); break;
    default: rounded = floor(b); break;
    }

    if (fp_is_nan(b)) {
        if (fp_is_snan(b))
            fpu_set_exception(c, FPSCR_VXSNAN);
        value = 0x80000000u;
        fpu_set_exception(c, FPSCR_VXCVI);
        exception = 1;
    } else if (rounded >= 2147483648.0) {
        value = 0x7FFFFFFFu;
        fpu_set_exception(c, FPSCR_VXCVI);
        exception = 1;
    } else if (rounded < -2147483648.0) {
        value = 0x80000000u;
        fpu_set_exception(c, FPSCR_VXCVI);
        exception = 1;
    } else {
        const int32_t sv = (int32_t)rounded;
        value = (uint32_t)sv;
        const double di = (double)sv;
        if (di == b) {
            fpu_clear_fifr(c);
        } else {
            set_fi(c, 1);
            c->fpscr = fabs(di) > fabs(b) ? (c->fpscr | FPSCR_FR) : (c->fpscr & ~FPSCR_FR);
        }
    }
    if (exception)
        fpu_clear_fifr(c);

    if (!exception || !(c->fpscr & FPSCR_VE)) {
        /* Según pruebas en hardware: FPRF no cambia y la palabra alta queda así. */
        uint64_t r = 0xFFF8000000000000ull | value;
        if (value == 0 && (f64_bits(b) & DOUBLE_SIGN))
            r |= 0x100000000ull;
        c->fpr[FD(i)][0] = r;
    }
    rc1(c, i);
}

static void op_fctiw(gekko_t *c, uint32_t i)  { convert_to_integer(c, i, c->fpscr & FPSCR_RN); }
static void op_fctiwz(gekko_t *c, uint32_t i) { convert_to_integer(c, i, 1); }

/* ---- comparaciones ------------------------------------------------------------- */

static void op_fcmpu(gekko_t *c, uint32_t i) { fpu_compare(c, CRFD(i), ps0(c, FA(i)), ps0(c, FB(i)), 0); }
static void op_fcmpo(gekko_t *c, uint32_t i) { fpu_compare(c, CRFD(i), ps0(c, FA(i)), ps0(c, FB(i)), 1); }

/* ---- FPSCR ----------------------------------------------------------------------- */

static void fpscr_updated(gekko_t *c)
{
    fpu_update_summary(c);
    gekko_fpu_sync(c);
}

static void op_mffs(gekko_t *c, uint32_t i)
{
    c->fpr[FD(i)][0] = 0xFFF8000000000000ull | c->fpscr;
    rc1(c, i);
}

static void op_mtfsb0(gekko_t *c, uint32_t i)
{
    c->fpscr &= ~(0x80000000u >> RD(i));
    fpscr_updated(c);
    rc1(c, i);
}

static void op_mtfsb1(gekko_t *c, uint32_t i)
{
    const uint32_t b = (0x80000000u >> RD(i)) & FPSCR_MASK;
    if (b & FPSCR_ANY_X)
        fpu_set_exception(c, b);
    else
        c->fpscr |= b;
    fpscr_updated(c);
    rc1(c, i);
}

static void op_mtfsfi(gekko_t *c, uint32_t i)
{
    const uint32_t shift = 4 * CRFD(i);
    const uint32_t imm = (i << 16) & 0xF0000000u;
    c->fpscr = ((c->fpscr & ~(0xF0000000u >> shift)) | (imm >> shift)) & FPSCR_MASK;
    fpscr_updated(c);
    rc1(c, i);
}

static void op_mtfsf(gekko_t *c, uint32_t i)
{
    const uint32_t fm = (i >> 17) & 0xFFu;
    uint32_t m = 0;
    for (uint32_t k = 0; k < 8; k++)
        if (fm & (1u << k))
            m |= 0xFu << (k * 4);
    c->fpscr = ((c->fpscr & ~m) | ((uint32_t)c->fpr[FB(i)][0] & m)) & FPSCR_MASK;
    fpscr_updated(c);
    rc1(c, i);
}

static void op_mcrfs(gekko_t *c, uint32_t i)
{
    const uint32_t shift = 4 * (7 - CRFS(i));
    const uint32_t flags = (c->fpscr >> shift) & 0xFu;
    c->fpscr &= ~((0xFu << shift) & (FPSCR_FX | FPSCR_ANY_X));   /* los leídos se borran */
    fpscr_updated(c);
    cr_set(c, CRFD(i), flags);
}

/* ---- cargas y almacenamientos -------------------------------------------------- */

/* Una dirección que no es múltiplo de 4 da excepción de alineación. */
static int misaligned(gekko_t *c, uint32_t ea)
{
    if (!(ea & 3u))
        return 0;
    c->spr[SPR_DAR] = ea;
    gekko_exception(c, EXC_ALIGNMENT, c->pc, 0);
    return 1;
}

#define EA_D(c, i)  ea_d(c, i)
#define EA_DU(c, i) ((c)->gpr[RA(i)] + (uint32_t)SIMM(i))
#define EA_X(c, i)  ea_x(c, i)
#define EA_XU(c, i) ((c)->gpr[RA(i)] + (c)->gpr[RB(i)])

#define FP_MEM(name, ea_expr, update, body)                                         \
    static void name(gekko_t *c, uint32_t i)                                        \
    {                                                                               \
        if (gekko_fp_unavailable(c))                                                \
            return;                                                                 \
        const uint32_t ea = ea_expr(c, i);                                          \
        if (misaligned(c, ea))                                                      \
            return;                                                                 \
        body;                                                                       \
        if (update)                                                                 \
            c->gpr[RA(i)] = ea;                                                     \
    }

#define LOAD_S  do { const uint64_t v = fpu_to_double(gc_read32(c->mem, ea)); \
                     c->fpr[FD(i)][0] = c->fpr[FD(i)][1] = v; } while (0)
#define LOAD_D  c->fpr[FD(i)][0] = gc_read64(c->mem, ea)
#define STORE_S gc_write32(c->mem, ea, fpu_to_single(c->fpr[FS(i)][0]))
#define STORE_D gc_write64(c->mem, ea, c->fpr[FS(i)][0])

FP_MEM(op_lfs,    EA_D,  0, LOAD_S)
FP_MEM(op_lfsu,   EA_DU, 1, LOAD_S)
FP_MEM(op_lfd,    EA_D,  0, LOAD_D)
FP_MEM(op_lfdu,   EA_DU, 1, LOAD_D)
FP_MEM(op_stfs,   EA_D,  0, STORE_S)
FP_MEM(op_stfsu,  EA_DU, 1, STORE_S)
FP_MEM(op_stfd,   EA_D,  0, STORE_D)
FP_MEM(op_stfdu,  EA_DU, 1, STORE_D)
FP_MEM(op_lfsx,   EA_X,  0, LOAD_S)
FP_MEM(op_lfsux,  EA_XU, 1, LOAD_S)
FP_MEM(op_lfdx,   EA_X,  0, LOAD_D)
FP_MEM(op_lfdux,  EA_XU, 1, LOAD_D)
FP_MEM(op_stfsx,  EA_X,  0, STORE_S)
FP_MEM(op_stfsux, EA_XU, 1, STORE_S)
FP_MEM(op_stfdx,  EA_X,  0, STORE_D)
FP_MEM(op_stfdux, EA_XU, 1, STORE_D)
FP_MEM(op_stfiwx, EA_X,  0, gc_write32(c->mem, ea, (uint32_t)c->fpr[FS(i)][0]))

void gekko_register_fpu(void)
{
    gekko_op_primary[48] = op_lfs;
    gekko_op_primary[49] = op_lfsu;
    gekko_op_primary[50] = op_lfd;
    gekko_op_primary[51] = op_lfdu;
    gekko_op_primary[52] = op_stfs;
    gekko_op_primary[53] = op_stfsu;
    gekko_op_primary[54] = op_stfd;
    gekko_op_primary[55] = op_stfdu;

    gekko_op_31[535] = op_lfsx;
    gekko_op_31[567] = op_lfsux;
    gekko_op_31[599] = op_lfdx;
    gekko_op_31[631] = op_lfdux;
    gekko_op_31[663] = op_stfsx;
    gekko_op_31[695] = op_stfsux;
    gekko_op_31[727] = op_stfdx;
    gekko_op_31[759] = op_stfdux;
    gekko_op_31[983] = op_stfiwx;

    gekko_op_59[18] = op_fdivs;
    gekko_op_59[20] = op_fsubs;
    gekko_op_59[21] = op_fadds;
    gekko_op_59[24] = op_fres;
    gekko_op_59[25] = op_fmuls;
    gekko_op_59[28] = op_fmsubs;
    gekko_op_59[29] = op_fmadds;
    gekko_op_59[30] = op_fnmsubs;
    gekko_op_59[31] = op_fnmadds;

    gekko_op_63a[18] = op_fdiv;
    gekko_op_63a[20] = op_fsub;
    gekko_op_63a[21] = op_fadd;
    gekko_op_63a[23] = op_fsel;
    gekko_op_63a[25] = op_fmul;
    gekko_op_63a[26] = op_frsqrte;
    gekko_op_63a[28] = op_fmsub;
    gekko_op_63a[29] = op_fmadd;
    gekko_op_63a[30] = op_fnmsub;
    gekko_op_63a[31] = op_fnmadd;

    gekko_op_63[0] = op_fcmpu;
    gekko_op_63[32] = op_fcmpo;
    gekko_op_63[12] = op_frsp;
    gekko_op_63[14] = op_fctiw;
    gekko_op_63[15] = op_fctiwz;
    gekko_op_63[40] = op_fneg;
    gekko_op_63[72] = op_fmr;
    gekko_op_63[136] = op_fnabs;
    gekko_op_63[264] = op_fabs;
    gekko_op_63[64] = op_mcrfs;
    gekko_op_63[583] = op_mffs;
    gekko_op_63[70] = op_mtfsb0;
    gekko_op_63[38] = op_mtfsb1;
    gekko_op_63[134] = op_mtfsfi;
    gekko_op_63[711] = op_mtfsf;
}
