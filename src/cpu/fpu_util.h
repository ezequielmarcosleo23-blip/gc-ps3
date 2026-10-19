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

/*
 * Interno del intérprete: aritmética de la FPU del Gekko.
 *
 * Portado de Dolphin (commit cf5cb7ef, GPL-2.0-or-later):
 *   Source/Core/Common/FloatUtils.{h,cpp}           clasificación, fres, frsqrte
 *   Source/Core/Core/PowerPC/Interpreter/Interpreter_FPUtils.h
 *                                                   NaN, FPSCR, Force25Bit,
 *                                                   madd de precisión simple,
 *                                                   conversiones de lfs/stfs
 * Mismos resultados bit a bit; cambia la forma (C11 en lugar de C++20).
 *
 * El redondeo lo hace la FPU de la máquina con el modo de FPSCR[RN]
 * (gekko_fpu_sync). Por eso src/cpu se compila con -ffp-contract=off (que el
 * compilador no fusione a*b+c) y -frounding-math (que respete el modo).
 */
#ifndef GCPS3_CPU_FPU_UTIL_H
#define GCPS3_CPU_FPU_UTIL_H

#include <stdint.h>
#include <string.h>

#include "cpu/gekko.h"

#define DOUBLE_SIGN  0x8000000000000000ull
#define DOUBLE_EXP   0x7FF0000000000000ull
#define DOUBLE_FRAC  0x000FFFFFFFFFFFFFull
#define DOUBLE_QBIT  0x0008000000000000ull
#define FLOAT_SIGN   0x80000000u
#define FLOAT_EXP    0x7F800000u
#define FLOAT_FRAC   0x007FFFFFu
#define PPC_NAN_BITS 0x7FF8000000000000ull     /* NaN por omisión del PowerPC */

/* Clases de FPSCR[FPRF] */
enum {
    FPCLASS_QNAN = 0x11, FPCLASS_NINF = 0x09, FPCLASS_NN = 0x08, FPCLASS_ND = 0x18,
    FPCLASS_NZ = 0x12, FPCLASS_PZ = 0x02, FPCLASS_PD = 0x14, FPCLASS_PN = 0x04,
    FPCLASS_PINF = 0x05,
};

/* FPCC (los 4 bits bajos de FPRF y el campo de CR de las comparaciones) */
enum { FPCC_FL = 8, FPCC_FG = 4, FPCC_FE = 2, FPCC_FU = 1 };

/* ---- bits <-> valores -------------------------------------------------------- */

static inline uint64_t f64_bits(double d)  { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double   f64_of(uint64_t u)  { double d; memcpy(&d, &u, 8); return d; }
static inline uint32_t f32_bits(float f)   { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float    f32_of(uint32_t u)  { float f; memcpy(&f, &u, 4); return f; }

static inline int bits_is_nan(uint64_t u)  { return (u & ~DOUBLE_SIGN) > DOUBLE_EXP; }
static inline int bits_is_inf(uint64_t u)  { return (u & ~DOUBLE_SIGN) == DOUBLE_EXP; }
static inline int bits_is_snan(uint64_t u) { return bits_is_nan(u) && !(u & DOUBLE_QBIT); }

static inline int    fp_is_nan(double d)   { return bits_is_nan(f64_bits(d)); }
static inline int    fp_is_inf(double d)   { return bits_is_inf(f64_bits(d)); }
static inline int    fp_is_snan(double d)  { return bits_is_snan(f64_bits(d)); }
static inline int    f32_is_nan(float f)   { return (f32_bits(f) & ~FLOAT_SIGN) > FLOAT_EXP; }
static inline double fp_make_quiet(double d) { return f64_of(f64_bits(d) | DOUBLE_QBIT); }
static inline double fp_ppc_nan(void)      { return f64_of(PPC_NAN_BITS); }

/* ---- registros de punto flotante ------------------------------------------- */

static inline double ps0(const gekko_t *c, uint32_t r) { return f64_of(c->fpr[r][0]); }
static inline double ps1(const gekko_t *c, uint32_t r) { return f64_of(c->fpr[r][1]); }

static inline void set_ps0(gekko_t *c, uint32_t r, double v) { c->fpr[r][0] = f64_bits(v); }

static inline void set_both(gekko_t *c, uint32_t r, double v0, double v1)
{
    c->fpr[r][0] = f64_bits(v0);
    c->fpr[r][1] = f64_bits(v1);
}

/* Resultado de precisión simple: va a ps0 y ps1 (Fill en Dolphin). */
static inline void fill_single(gekko_t *c, uint32_t r, float v)
{
    const uint64_t u = f64_bits((double)v);
    c->fpr[r][0] = u;
    c->fpr[r][1] = u;
}

/* ---- FPSCR ------------------------------------------------------------------ */

/* VX y FEX salen del resto; con FEX y MSR[FE0|FE1], excepción de programa. */
void fpu_update_summary(gekko_t *c);

/* Pone los bits de excepción `mask` (y FX si alguno estaba en 0). */
void fpu_set_exception(gekko_t *c, uint32_t mask);

static inline void fpu_clear_fifr(gekko_t *c) { c->fpscr &= ~(FPSCR_FI | FPSCR_FR); }

static inline void fpu_set_fprf(gekko_t *c, uint32_t cls)
{
    c->fpscr = (c->fpscr & ~FPSCR_FPRF) | (cls << 12);
}

uint32_t fpu_classify_double(double d);
uint32_t fpu_classify_float(float f);

static inline void fpu_fprf_double(gekko_t *c, double d) { fpu_set_fprf(c, fpu_classify_double(d)); }
static inline void fpu_fprf_single(gekko_t *c, float f)  { fpu_set_fprf(c, fpu_classify_float(f)); }

/* CR1 = FX, FEX, VX, OX (Rc = 1 de las instrucciones de punto flotante) */
static inline void fpu_cr1(gekko_t *c)
{
    c->cr = (c->cr & ~0x0F000000u) | ((c->fpscr >> 4) & 0x0F000000u);
}

/* fcmpu/fcmpo y ps_cmp*: FPCC en FPSCR y en el campo crf de CR. `ordered`
 * agrega VXVC con NaN (fcmpo). */
void fpu_compare(gekko_t *c, uint32_t crf, double a, double b, int ordered);

/* ---- redondeo y conversiones ----------------------------------------------- */

/* Cambia el modo de redondeo de la máquina (0-3, como FPSCR[RN]). */
void fpu_host_round(uint32_t rn);

/* Redondeo a simple; con FPSCR[NI], los resultados subnormales van a cero. */
float  fpu_force_single(uint32_t fpscr, double v);
double fpu_force_double(uint32_t fpscr, double v);

/* frC de fmuls/fmadds y paired singles: mantisa redondeada a 25 bits. */
double fpu_force25bit(double d);

uint32_t fpu_to_single(uint64_t x);        /* stfs */
uint32_t fpu_to_single_ftz(uint64_t x);    /* psq_st: subnormales a cero */
uint64_t fpu_to_double(uint32_t x);        /* lfs, psq_l */

double fpu_approx_recip(double v);         /* fres, ps_res */
double fpu_approx_rsqrt(double v);         /* frsqrte, ps_rsqrte */

/* ---- operaciones con NaN y excepciones del PowerPC ------------------------- */

typedef struct fp_result {
    double   value;
    uint32_t exception;        /* el último bit de FPSCR que puso la operación */
} fp_result_t;

static inline int fp_no_invalid(fp_result_t r) { return (r.exception & FPSCR_VX_ANY) == 0; }

fp_result_t fpu_mul(gekko_t *c, double a, double b);
fp_result_t fpu_div(gekko_t *c, double a, double b);
fp_result_t fpu_add(gekko_t *c, double a, double b);
fp_result_t fpu_sub(gekko_t *c, double a, double b);
/* a * c ± b, con NaN en orden a, b, c. `single`: frC a 25 bits y un solo
 * redondeo a simple (corrección de empates de Dolphin). */
fp_result_t fpu_madd(gekko_t *c, double a, double cc, double b, int sub, int single);

#endif
