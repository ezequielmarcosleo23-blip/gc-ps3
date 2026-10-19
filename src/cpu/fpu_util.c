/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008-2018 Dolphin Emulator Project
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

/* Ver fpu_util.h: qué viene de Dolphin y por qué se compila sin contracción. */
#include "cpu/fpu_util.h"

#if !defined(__powerpc__) && !defined(__PPU__)
#include <fenv.h>
#endif

#include "cpu/interp.h"

/* a * b + c con un solo redondeo. En PowerPC es fmadd; en x86-64 sin FMA,
 * la fma() de glibc (exacta). No se usa la fma() de newlib: hace a * b + c. */
#define FMA(a, b, c) __builtin_fma((a), (b), (c))

/* ---- FPSCR ------------------------------------------------------------------ */

void fpu_update_summary(gekko_t *c)
{
    uint32_t f = c->fpscr & ~(FPSCR_VX | FPSCR_FEX);
    if (f & FPSCR_VX_ANY)
        f |= FPSCR_VX;
    if ((f >> 22) & (f & FPSCR_ANY_E))
        f |= FPSCR_FEX;
    c->fpscr = f;
    if ((f & FPSCR_FEX) && (c->msr & (MSR_FE0 | MSR_FE1)))
        gekko_exception(c, EXC_PROGRAM, c->pc, SRR1_PROGRAM_FP);
}

void fpu_set_exception(gekko_t *c, uint32_t mask)
{
    if ((c->fpscr & mask) != mask)
        c->fpscr |= FPSCR_FX;
    c->fpscr |= mask;
    fpu_update_summary(c);
}

void fpu_compare(gekko_t *c, uint32_t crf, double a, double b, int ordered)
{
    uint32_t cc;
    if (fp_is_nan(a) || fp_is_nan(b)) {
        cc = FPCC_FU;
        if (fp_is_snan(a) || fp_is_snan(b)) {
            fpu_set_exception(c, FPSCR_VXSNAN);
            if (ordered && !(c->fpscr & FPSCR_VE))
                fpu_set_exception(c, FPSCR_VXVC);
        } else if (ordered) {
            fpu_set_exception(c, FPSCR_VXVC);
        }
    } else if (a < b) {
        cc = FPCC_FL;
    } else if (a > b) {
        cc = FPCC_FG;
    } else {
        cc = FPCC_FE;
    }
    c->fpscr = (c->fpscr & ~FPSCR_FPCC) | (cc << 12);
    cr_set(c, crf, cc);
}

uint32_t fpu_classify_double(double d)
{
    const uint64_t u = f64_bits(d);
    const uint64_t sign = u & DOUBLE_SIGN, exp = u & DOUBLE_EXP;
    if (exp > 0 && exp < DOUBLE_EXP)
        return sign ? FPCLASS_NN : FPCLASS_PN;
    if (u & DOUBLE_FRAC) {
        if (exp)
            return FPCLASS_QNAN;
        return sign ? FPCLASS_ND : FPCLASS_PD;
    }
    if (exp)
        return sign ? FPCLASS_NINF : FPCLASS_PINF;
    return sign ? FPCLASS_NZ : FPCLASS_PZ;
}

uint32_t fpu_classify_float(float f)
{
    const uint32_t u = f32_bits(f);
    const uint32_t sign = u & FLOAT_SIGN, exp = u & FLOAT_EXP;
    if (exp > 0 && exp < FLOAT_EXP)
        return sign ? FPCLASS_NN : FPCLASS_PN;
    if (u & FLOAT_FRAC) {
        if (exp)
            return FPCLASS_QNAN;
        return sign ? FPCLASS_ND : FPCLASS_PD;
    }
    if (exp)
        return sign ? FPCLASS_NINF : FPCLASS_PINF;
    return sign ? FPCLASS_NZ : FPCLASS_PZ;
}

/* ---- modo de redondeo de la máquina ----------------------------------------- */

void fpu_host_round(uint32_t rn)
{
#if defined(__powerpc__) || defined(__PPU__)
    /* Campo 7 de FPSCR = XE, NI, RN: la máquina queda en IEEE, sin trampas. */
    switch (rn & 3) {
    case 0: __asm__ volatile("mtfsfi 7,0" ::: "memory"); break;
    case 1: __asm__ volatile("mtfsfi 7,1" ::: "memory"); break;
    case 2: __asm__ volatile("mtfsfi 7,2" ::: "memory"); break;
    default: __asm__ volatile("mtfsfi 7,3" ::: "memory"); break;
    }
#else
    static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    fesetround(modes[rn & 3]);
#endif
}

void gekko_fpu_sync(const gekko_t *c)
{
    fpu_host_round(c->fpscr & FPSCR_RN);
}

/* ---- redondeo y conversiones ----------------------------------------------- */

static float flush_float(float f)
{
    uint32_t u = f32_bits(f);
    if ((u & FLOAT_EXP) == 0)
        u &= FLOAT_SIGN;
    return f32_of(u);
}

static double flush_double(double d)
{
    uint64_t u = f64_bits(d);
    if ((u & DOUBLE_EXP) == 0)
        u &= DOUBLE_SIGN;
    return f64_of(u);
}

float fpu_force_single(uint32_t fpscr, double v)
{
    if (fpscr & FPSCR_NI) {
        /* Con NI, lo que antes de redondear es subnormal en simple va a cero,
         * aunque al redondear llegara a normal. */
        const uint64_t u = f64_bits(v);
        if ((u & ~DOUBLE_SIGN) < 0x3810000000000000ull)
            return f32_of((uint32_t)((u & DOUBLE_SIGN) >> 32));
    }
    float x = (float)v;
    if (fpscr & FPSCR_NI)
        x = flush_float(x);
    return x;
}

double fpu_force_double(uint32_t fpscr, double v)
{
    return (fpscr & FPSCR_NI) ? flush_double(v) : v;
}

double fpu_force25bit(double d)
{
    uint64_t u = f64_bits(d);
    const uint64_t exp = u & DOUBLE_EXP, frac = u & DOUBLE_FRAC;

    if (exp == 0 && frac != 0) {
        /* Los subnormales se "normalizan" antes de redondear: el bit de
         * redondeo se corre tanto como ceros tenga la fracción adelante. */
        int64_t keep = (int64_t)0xFFFFFFFFF8000000ull;
        uint64_t round = 0x8000000ull;
        const int shift = __builtin_clzll(frac) - (63 - 52);
        keep >>= shift;                     /* aritmético: repite los unos */
        round >>= shift;
        u = (u & (uint64_t)keep) + (u & round);
    } else {
        u = (u & 0xFFFFFFFFF8000000ull) + (u & 0x8000000ull);
    }
    return f64_of(u);
}

uint32_t fpu_to_single(uint64_t x)
{
    const uint32_t exp = (uint32_t)((x >> 52) & 0x7FF);

    if (exp > 896 || (x & ~DOUBLE_SIGN) == 0)
        return (uint32_t)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    if (exp >= 874) {
        uint32_t t = (uint32_t)(0x80000000u | ((x & DOUBLE_FRAC) >> 21));
        t >>= 905 - exp;
        t |= (uint32_t)((x >> 32) & 0x80000000u);
        return t;
    }
    /* Sin definir en la arquitectura; Dolphin lo tomó de pruebas en hardware. */
    return (uint32_t)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
}

uint32_t fpu_to_single_ftz(uint64_t x)
{
    const uint32_t exp = (uint32_t)((x >> 52) & 0x7FF);

    if (exp > 896 || (x & ~DOUBLE_SIGN) == 0)
        return (uint32_t)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    return (uint32_t)((x >> 32) & 0x80000000u);
}

uint64_t fpu_to_double(uint32_t value)
{
    /* Algoritmo de lfs del manual de entornos de programación del PowerPC. */
    const uint64_t x = value;
    uint64_t exp = (x >> 23) & 0xFF;
    uint64_t frac = x & 0x007FFFFF;

    if (exp > 0 && exp < 255) {
        const uint64_t y = !(exp >> 7);
        const uint64_t z = y << 61 | y << 60 | y << 59;
        return ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
    }
    if (exp == 0 && frac != 0) {           /* subnormal: se normaliza */
        exp = 1023 - 126;
        do {
            frac <<= 1;
            exp -= 1;
        } while ((frac & 0x00800000u) == 0);
        return ((x & 0x80000000u) << 32) | (exp << 52) | ((frac & 0x007FFFFFu) << 29);
    }
    /* NaN, infinito o cero */
    const uint64_t y = exp >> 7;
    const uint64_t z = y << 61 | y << 60 | y << 59;
    return ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
}

/* ---- fres y frsqrte: las tablas del hardware -------------------------------- */

typedef struct base_dec {
    int32_t base;
    int32_t dec;
} base_dec_t;

static const base_dec_t frsqrte_expected[32] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};

static const base_dec_t fres_expected[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313},
    {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261},
    {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5},
    {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143},
    {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
};

#define F64_INF      0x7FF0000000000000ull
#define F32_MAX_AS64 0x47EFFFFFE0000000ull   /* FLT_MAX como double */

double fpu_approx_rsqrt(double val)
{
    int64_t integral = (int64_t)f64_bits(val);
    int64_t mantissa = integral & ((1ll << 52) - 1);
    const int64_t sign = (int64_t)((uint64_t)integral & DOUBLE_SIGN);
    int64_t exponent = integral & (0x7FFll << 52);

    if (mantissa == 0 && exponent == 0)
        return f64_of(sign ? (F64_INF | DOUBLE_SIGN) : F64_INF);
    if (exponent == (0x7FFll << 52)) {
        if (mantissa == 0)
            return sign ? fp_ppc_nan() : 0.0;
        return fp_make_quiet(val);
    }
    if (sign)
        return fp_ppc_nan();

    if (!exponent) {                        /* subnormal: se "normaliza" */
        do {
            exponent -= 1ll << 52;
            mantissa <<= 1;
        } while (!(mantissa & (1ll << 52)));
        mantissa &= (1ll << 52) - 1;
        exponent += 1ll << 52;
    }

    const int64_t exponent_lsb = exponent & (1ll << 52);
    exponent = ((0x3FFll << 52) - ((exponent - (0x3FEll << 52)) / 2)) & (0x7FFll << 52);
    integral = sign | exponent;

    const int i = (int)((exponent_lsb | mantissa) >> 37);
    const base_dec_t *e = &frsqrte_expected[i / 2048];
    integral |= (int64_t)(e->base + e->dec * (i % 2048)) << 26;
    return f64_of((uint64_t)integral);
}

double fpu_approx_recip(double val)
{
    int64_t integral = (int64_t)f64_bits(val);
    const int64_t mantissa = integral & ((1ll << 52) - 1);
    const uint64_t sign = (uint64_t)integral & DOUBLE_SIGN;
    int64_t exponent = integral & (0x7FFll << 52);

    if (mantissa == 0 && exponent == 0)
        return f64_of(sign | F64_INF);
    if (exponent == (0x7FFll << 52)) {
        if (mantissa == 0)
            return f64_of(sign);            /* ±0 */
        return fp_make_quiet(val);
    }
    if (exponent < (895ll << 52))           /* entradas muy chicas: ±FLT_MAX */
        return f64_of(sign | F32_MAX_AS64);
    if (exponent >= (1149ll << 52))         /* muy grandes: ±0 */
        return f64_of(sign);

    exponent = (0x7FDll << 52) - exponent;
    const int i = (int)(mantissa >> 37);
    const base_dec_t *e = &fres_expected[i / 1024];
    integral = (int64_t)sign | exponent;
    integral |= (int64_t)(e->base - (e->dec * (i % 1024) + 1) / 2) << 29;
    return f64_of((uint64_t)integral);
}

/* ---- operaciones -------------------------------------------------------------- */

static fp_result_t res(double v)
{
    fp_result_t r = { v, 0 };
    return r;
}

static void res_exception(gekko_t *c, fp_result_t *r, uint32_t flag)
{
    r->exception = flag;
    fpu_set_exception(c, flag);
}

/* Resultado NaN: SNaN -> VXSNAN; si una entrada es NaN, sale esa (silenciada);
 * si no, el NaN por omisión con la excepción de operación inválida `invalid`. */
static fp_result_t nan_result(gekko_t *c, fp_result_t r, double a, double b, uint32_t invalid)
{
    if (fp_is_snan(a) || fp_is_snan(b))
        res_exception(c, &r, FPSCR_VXSNAN);
    fpu_clear_fifr(c);
    if (fp_is_nan(a)) {
        r.value = fp_make_quiet(a);
        return r;
    }
    if (fp_is_nan(b)) {
        r.value = fp_make_quiet(b);
        return r;
    }
    if (invalid)
        res_exception(c, &r, invalid);
    r.value = fp_ppc_nan();
    return r;
}

fp_result_t fpu_mul(gekko_t *c, double a, double b)
{
    fp_result_t r = res(a * b);
    if (fp_is_nan(r.value))
        return nan_result(c, r, a, b, FPSCR_VXIMZ);
    return r;
}

fp_result_t fpu_div(gekko_t *c, double a, double b)
{
    fp_result_t r = res(a / b);
    if (fp_is_inf(r.value)) {
        /* ZX solo con dividendo finito: ∞/0 da ∞ sin excepción (arquitectura;
         * Dolphin pone ZX, D-030) */
        if (b == 0.0 && !fp_is_inf(a))
            res_exception(c, &r, FPSCR_ZX);
        return r;
    }
    if (fp_is_nan(r.value)) {
        if (fp_is_snan(a) || fp_is_snan(b))
            res_exception(c, &r, FPSCR_VXSNAN);
        fpu_clear_fifr(c);
        if (fp_is_nan(a)) {
            r.value = fp_make_quiet(a);
            return r;
        }
        if (fp_is_nan(b)) {
            r.value = fp_make_quiet(b);
            return r;
        }
        if (b == 0.0)
            res_exception(c, &r, FPSCR_VXZDZ);
        else if (fp_is_inf(a) && fp_is_inf(b))
            res_exception(c, &r, FPSCR_VXIDI);
        r.value = fp_ppc_nan();
    }
    return r;
}

static fp_result_t add_sub(gekko_t *c, double a, double b, double v)
{
    fp_result_t r = res(v);
    if (fp_is_nan(r.value))
        return nan_result(c, r, a, b, FPSCR_VXISI);
    if (fp_is_inf(a) || fp_is_inf(b))
        fpu_clear_fifr(c);
    return r;
}

fp_result_t fpu_add(gekko_t *c, double a, double b) { return add_sub(c, a, b, a + b); }
fp_result_t fpu_sub(gekko_t *c, double a, double b) { return add_sub(c, a, b, a - b); }

fp_result_t fpu_madd(gekko_t *c, double a, double cc, double b, int sub, int single)
{
    fp_result_t r = res(0.0);
    const double b_sign = sub ? -b : b;

    if (!single) {
        r.value = FMA(a, cc, b_sign);
    } else {
        /* Precisión simple: frC redondeado a 25 bits y un FMA de 64 bits. Si
         * el resultado queda justo en un empate al pasarlo a simple, se mide
         * el error del FMA (2Sum de Møller) y se corrige hacia dónde
         * redondear. Ver el comentario largo de NI_madd_msub en Dolphin. */
        const double c_round = fpu_force25bit(cc);
        r.value = FMA(a, c_round, b_sign);

        const uint64_t bits = f64_bits(r.value);
        const uint64_t D_MASK = 0x000000001FFFFFFFull;
        const uint64_t EVEN_TIE = 0x0000000010000000ull;
        if ((bits & D_MASK) == EVEN_TIE) {
            const double a_prime = b_sign - r.value;
            const double b_prime = r.value + a_prime;
            const double delta_a = FMA(a, c_round, a_prime);
            const double delta_b = b_sign - b_prime;
            const double error = delta_a + delta_b;
            if (error != 0.0) {
                if ((error > 0.0) == (r.value > 0.0))
                    r.value = f64_of(bits + 1);     /* el empate se queda corto */
                else
                    r.value = f64_of(bits - 1);     /* se pasa */
            }
        }
    }

    if (fp_is_nan(r.value)) {
        /* ∞ × 0 es inválida aunque frB sea NaN (arquitectura; Dolphin no
         * pone VXIMZ en ese caso, D-030). El resultado sigue siendo frB. */
        const int imz = !fp_is_nan(a) && !fp_is_nan(cc) && fp_is_nan(a * cc);
        if (fp_is_snan(a) || fp_is_snan(b) || fp_is_snan(cc))
            res_exception(c, &r, FPSCR_VXSNAN);
        fpu_clear_fifr(c);
        if (imz)
            res_exception(c, &r, FPSCR_VXIMZ);
        if (fp_is_nan(a)) {
            r.value = fp_make_quiet(a);
            return r;
        }
        if (fp_is_nan(b)) {
            r.value = fp_make_quiet(b);
            return r;
        }
        if (fp_is_nan(cc)) {
            r.value = fp_make_quiet(cc);
            return r;
        }
        if (!imz)
            res_exception(c, &r, FPSCR_VXISI);
        r.value = fp_ppc_nan();
        return r;
    }
    if (fp_is_inf(a) || fp_is_inf(b) || fp_is_inf(cc))
        fpu_clear_fifr(c);
    return r;
}
