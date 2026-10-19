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
 * Paired singles del Gekko: las instrucciones del opcode 4 (dos floats por
 * registro) y las cargas/almacenamientos cuantizados psq_l/psq_st con GQR.
 *
 * Semántica de Dolphin (commit cf5cb7ef): Interpreter_Paired.cpp e
 * Interpreter_LoadStorePaired.cpp. Cada mitad se calcula como su equivalente
 * escalar de precisión simple (ps_add = fadds en ps0 y en ps1); FPRF sale de
 * ps0 (de ps1 en ps_sum1). A diferencia de las escalares, siempre escriben el
 * destino aunque FPSCR[VE] esté prendido.
 *
 * HID2[PSE] = 0 hace ilegales las del opcode 4 (lo revisa gekko.c) y psq_l,
 * psq_lu, psq_st y psq_stu; estas cuatro piden además HID2[LSQE].
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

static inline float single(const gekko_t *c, fp_result_t r) { return fpu_force_single(c->fpscr, r.value); }

static inline float negf_unless_nan(float v)
{
    return f32_is_nan(v) ? v : f32_of(f32_bits(v) ^ FLOAT_SIGN);
}

/* Escribe ps0 y ps1, FPRF según `fprf` y CR1 si Rc. */
static void put_pair(gekko_t *c, uint32_t i, float v0, float v1, float fprf)
{
    set_both(c, FD(i), (double)v0, (double)v1);
    fpu_fprf_single(c, fprf);
    rc1(c, i);
}

/* ---- aritmética --------------------------------------------------------------- */

static void ps_add(gekko_t *c, uint32_t i)
{
    const uint32_t a = FA(i), b = FB(i);
    const float v0 = single(c, fpu_add(c, ps0(c, a), ps0(c, b)));
    const float v1 = single(c, fpu_add(c, ps1(c, a), ps1(c, b)));
    put_pair(c, i, v0, v1, v0);
}

static void ps_sub(gekko_t *c, uint32_t i)
{
    const uint32_t a = FA(i), b = FB(i);
    const float v0 = single(c, fpu_sub(c, ps0(c, a), ps0(c, b)));
    const float v1 = single(c, fpu_sub(c, ps1(c, a), ps1(c, b)));
    put_pair(c, i, v0, v1, v0);
}

static void ps_div(gekko_t *c, uint32_t i)
{
    const uint32_t a = FA(i), b = FB(i);
    const float v0 = single(c, fpu_div(c, ps0(c, a), ps0(c, b)));
    const float v1 = single(c, fpu_div(c, ps1(c, a), ps1(c, b)));
    put_pair(c, i, v0, v1, v0);
}

static void ps_mul(gekko_t *c, uint32_t i)
{
    const uint32_t a = FA(i), cc = FC(i);
    const double c0 = fpu_force25bit(ps0(c, cc)), c1 = fpu_force25bit(ps1(c, cc));
    const float v0 = single(c, fpu_mul(c, ps0(c, a), c0));
    const float v1 = single(c, fpu_mul(c, ps1(c, a), c1));
    put_pair(c, i, v0, v1, v0);
}

/* ps_madd y familia: frA * frC ± frB por mitades (frC a 25 bits dentro de fpu_madd). */
#define PS_MADD(name, sub, neg)                                                     \
    static void name(gekko_t *c, uint32_t i)                                        \
    {                                                                               \
        const uint32_t a = FA(i), b = FB(i), cc = FC(i);                            \
        float v0 = single(c, fpu_madd(c, ps0(c, a), ps0(c, cc), ps0(c, b), sub, 1)); \
        float v1 = single(c, fpu_madd(c, ps1(c, a), ps1(c, cc), ps1(c, b), sub, 1)); \
        if (neg) {                                                                  \
            v0 = negf_unless_nan(v0);                                               \
            v1 = negf_unless_nan(v1);                                               \
        }                                                                           \
        put_pair(c, i, v0, v1, v0);                                                 \
    }

PS_MADD(ps_madd, 0, 0)
PS_MADD(ps_msub, 1, 0)
PS_MADD(ps_nmadd, 0, 1)
PS_MADD(ps_nmsub, 1, 1)

/* ps_sum0: ps0 = a0 + b1, ps1 = c1.  ps_sum1: ps0 = c0, ps1 = a0 + b1. */
static void ps_sum0(gekko_t *c, uint32_t i)
{
    const float v0 = single(c, fpu_add(c, ps0(c, FA(i)), ps1(c, FB(i))));
    const float v1 = fpu_force_single(c->fpscr, ps1(c, FC(i)));
    put_pair(c, i, v0, v1, v0);
}

static void ps_sum1(gekko_t *c, uint32_t i)
{
    const float v0 = fpu_force_single(c->fpscr, ps0(c, FC(i)));
    const float v1 = single(c, fpu_add(c, ps0(c, FA(i)), ps1(c, FB(i))));
    put_pair(c, i, v0, v1, v1);
}

/* ps_muls0/1: las dos mitades de frA por la mitad 0/1 de frC. */
static void muls(gekko_t *c, uint32_t i, int half)
{
    const uint32_t a = FA(i);
    const double cv = fpu_force25bit(f64_of(c->fpr[FC(i)][half]));
    const float v0 = single(c, fpu_mul(c, ps0(c, a), cv));
    const float v1 = single(c, fpu_mul(c, ps1(c, a), cv));
    put_pair(c, i, v0, v1, v0);
}

static void ps_muls0(gekko_t *c, uint32_t i) { muls(c, i, 0); }
static void ps_muls1(gekko_t *c, uint32_t i) { muls(c, i, 1); }

/* ps_madds0/1: frA * (mitad 0/1 de frC) + frB, por mitades. */
static void madds(gekko_t *c, uint32_t i, int half)
{
    const uint32_t a = FA(i), b = FB(i);
    const double cv = f64_of(c->fpr[FC(i)][half]);
    const float v0 = single(c, fpu_madd(c, ps0(c, a), cv, ps0(c, b), 0, 1));
    const float v1 = single(c, fpu_madd(c, ps1(c, a), cv, ps1(c, b), 0, 1));
    put_pair(c, i, v0, v1, v0);
}

static void ps_madds0(gekko_t *c, uint32_t i) { madds(c, i, 0); }
static void ps_madds1(gekko_t *c, uint32_t i) { madds(c, i, 1); }

static void ps_res(gekko_t *c, uint32_t i)
{
    /* Según pruebas en hardware (Dolphin) */
    const double a = ps0(c, FB(i)), b = ps1(c, FB(i));

    if (a == 0.0 || b == 0.0) {
        fpu_set_exception(c, FPSCR_ZX);
        fpu_clear_fifr(c);
    }
    if (fp_is_nan(a) || fp_is_inf(a) || fp_is_nan(b) || fp_is_inf(b))
        fpu_clear_fifr(c);
    if (fp_is_snan(a) || fp_is_snan(b))
        fpu_set_exception(c, FPSCR_VXSNAN);

    const double v0 = fpu_approx_recip(a), v1 = fpu_approx_recip(b);
    set_both(c, FD(i), v0, v1);
    fpu_fprf_single(c, (float)v0);
    rc1(c, i);
}

static void ps_rsqrte(gekko_t *c, uint32_t i)
{
    const double a = ps0(c, FB(i)), b = ps1(c, FB(i));

    if (a == 0.0 || b == 0.0) {
        fpu_set_exception(c, FPSCR_ZX);
        fpu_clear_fifr(c);
    }
    if (a < 0.0 || b < 0.0) {
        fpu_set_exception(c, FPSCR_VXSQRT);
        fpu_clear_fifr(c);
    }
    if (fp_is_nan(a) || fp_is_inf(a) || fp_is_nan(b) || fp_is_inf(b))
        fpu_clear_fifr(c);
    if (fp_is_snan(a) || fp_is_snan(b))
        fpu_set_exception(c, FPSCR_VXSNAN);

    const float v0 = fpu_force_single(c->fpscr, fpu_approx_rsqrt(a));
    const float v1 = fpu_force_single(c->fpscr, fpu_approx_rsqrt(b));
    put_pair(c, i, v0, v1, v0);
}

/* ---- binarias: no tocan FPSCR ------------------------------------------------- */

static void ps_sel(gekko_t *c, uint32_t i)
{
    const uint32_t a = FA(i), b = FB(i), cc = FC(i);
    const uint64_t v0 = ps0(c, a) >= -0.0 ? c->fpr[cc][0] : c->fpr[b][0];
    const uint64_t v1 = ps1(c, a) >= -0.0 ? c->fpr[cc][1] : c->fpr[b][1];
    c->fpr[FD(i)][0] = v0;
    c->fpr[FD(i)][1] = v1;
    rc1(c, i);
}

#define PS_BITS(name, expr)                                                         \
    static void name(gekko_t *c, uint32_t i)                                        \
    {                                                                               \
        const uint64_t b0 = c->fpr[FB(i)][0], b1 = c->fpr[FB(i)][1];                \
        c->fpr[FD(i)][0] = (expr(b0));                                              \
        c->fpr[FD(i)][1] = (expr(b1));                                              \
        rc1(c, i);                                                                  \
    }

#define BITS_NEG(x)  ((x) ^ DOUBLE_SIGN)
#define BITS_ABS(x)  ((x) & ~DOUBLE_SIGN)
#define BITS_NABS(x) ((x) | DOUBLE_SIGN)
#define BITS_MR(x)   (x)

PS_BITS(ps_neg, BITS_NEG)
PS_BITS(ps_abs, BITS_ABS)
PS_BITS(ps_nabs, BITS_NABS)
PS_BITS(ps_mr, BITS_MR)

/* ps_mergeXY: ps0 = mitad X de frA, ps1 = mitad Y de frB */
#define PS_MERGE(name, x, y)                                                        \
    static void name(gekko_t *c, uint32_t i)                                        \
    {                                                                               \
        const uint64_t v0 = c->fpr[FA(i)][x], v1 = c->fpr[FB(i)][y];                \
        c->fpr[FD(i)][0] = v0;                                                      \
        c->fpr[FD(i)][1] = v1;                                                      \
        rc1(c, i);                                                                  \
    }

PS_MERGE(ps_merge00, 0, 0)
PS_MERGE(ps_merge01, 0, 1)
PS_MERGE(ps_merge10, 1, 0)
PS_MERGE(ps_merge11, 1, 1)

static void ps_cmpu0(gekko_t *c, uint32_t i) { fpu_compare(c, CRFD(i), ps0(c, FA(i)), ps0(c, FB(i)), 0); }
static void ps_cmpo0(gekko_t *c, uint32_t i) { fpu_compare(c, CRFD(i), ps0(c, FA(i)), ps0(c, FB(i)), 1); }
static void ps_cmpu1(gekko_t *c, uint32_t i) { fpu_compare(c, CRFD(i), ps1(c, FA(i)), ps1(c, FB(i)), 0); }
static void ps_cmpo1(gekko_t *c, uint32_t i) { fpu_compare(c, CRFD(i), ps1(c, FA(i)), ps1(c, FB(i)), 1); }

/* ---- psq_l / psq_st ------------------------------------------------------------ */

/* Escalas: ST multiplica por 2^scale, LD divide; scale va de -32 a 31 en 6 bits. */
static float quantize_scale[64];
static float dequantize_scale[64];

static void scales_init(void)
{
    for (int k = 0; k < 64; k++) {
        const int e = k < 32 ? k : k - 64;
        quantize_scale[k] = ldexpf(1.0f, e);
        dequantize_scale[k] = ldexpf(1.0f, -e);
    }
}

/* Escala, satura al rango del tipo y trunca hacia cero (un NaN da 0). */
static int32_t scale_and_clamp(double v, uint32_t scale, float lo, float hi)
{
    const float conv = (float)v * quantize_scale[scale];
    if (f32_is_nan(conv))
        return 0;
    if (conv < lo)
        return (int32_t)lo;
    if (hi < conv)
        return (int32_t)hi;
    return (int32_t)conv;
}

static void quantize(gekko_t *c, uint32_t ea, uint32_t gqr_idx, uint32_t rs, uint32_t w)
{
    const uint32_t gqr = c->spr[SPR_GQR0 + gqr_idx];
    const uint32_t scale = GQR_ST_SCALE(gqr);
    const double v0 = ps0(c, rs), v1 = ps1(c, rs);
    float lo, hi;
    unsigned bytes;

    switch (GQR_ST_TYPE(gqr)) {
    case GQR_FLOAT: {
        const uint32_t s0 = fpu_to_single_ftz(c->fpr[rs][0]);
        if (w)
            gc_write32(c->mem, ea, s0);
        else
            gc_write64(c->mem, ea, (uint64_t)s0 << 32 | fpu_to_single_ftz(c->fpr[rs][1]));
        return;
    }
    case GQR_U8:  lo = 0.0f;      hi = 255.0f;   bytes = 1; break;
    case GQR_U16: lo = 0.0f;      hi = 65535.0f; bytes = 2; break;
    case GQR_S8:  lo = -128.0f;   hi = 127.0f;   bytes = 1; break;
    case GQR_S16: lo = -32768.0f; hi = 32767.0f; bytes = 2; break;
    default:
        return;                             /* tipos 1-3: sin definir */
    }

    const uint32_t q0 = (uint32_t)scale_and_clamp(v0, scale, lo, hi);
    if (bytes == 1) {
        if (w)
            gc_write8(c->mem, ea, (uint8_t)q0);
        else
            gc_write16(c->mem, ea, (uint16_t)((q0 & 0xFFu) << 8 |
                                              ((uint32_t)scale_and_clamp(v1, scale, lo, hi) & 0xFFu)));
    } else {
        if (w)
            gc_write16(c->mem, ea, (uint16_t)q0);
        else
            gc_write32(c->mem, ea, (q0 & 0xFFFFu) << 16 |
                                   ((uint32_t)scale_and_clamp(v1, scale, lo, hi) & 0xFFFFu));
    }
}

static double dq(int32_t v, uint32_t scale)
{
    return (double)((float)v * dequantize_scale[scale]);
}

static void dequantize(gekko_t *c, uint32_t ea, uint32_t gqr_idx, uint32_t rd, uint32_t w)
{
    const uint32_t gqr = c->spr[SPR_GQR0 + gqr_idx];
    const uint32_t scale = GQR_LD_SCALE(gqr);
    double v0 = 0.0, v1 = 1.0;

    switch (GQR_LD_TYPE(gqr)) {
    case GQR_FLOAT:
        if (w) {
            c->fpr[rd][0] = fpu_to_double(gc_read32(c->mem, ea));
            c->fpr[rd][1] = f64_bits(1.0);
        } else {
            const uint64_t p = gc_read64(c->mem, ea);
            c->fpr[rd][0] = fpu_to_double((uint32_t)(p >> 32));
            c->fpr[rd][1] = fpu_to_double((uint32_t)p);
        }
        return;
    case GQR_U8:
        if (w) {
            v0 = dq(gc_read8(c->mem, ea), scale);
        } else {
            const uint16_t p = gc_read16(c->mem, ea);
            v0 = dq(p >> 8, scale);
            v1 = dq(p & 0xFF, scale);
        }
        break;
    case GQR_U16:
        if (w) {
            v0 = dq(gc_read16(c->mem, ea), scale);
        } else {
            const uint32_t p = gc_read32(c->mem, ea);
            v0 = dq((int32_t)(p >> 16), scale);
            v1 = dq((int32_t)(p & 0xFFFFu), scale);
        }
        break;
    case GQR_S8:
        if (w) {
            v0 = dq((int8_t)gc_read8(c->mem, ea), scale);
        } else {
            const uint16_t p = gc_read16(c->mem, ea);
            v0 = dq((int8_t)(p >> 8), scale);
            v1 = dq((int8_t)(p & 0xFF), scale);
        }
        break;
    case GQR_S16:
        if (w) {
            v0 = dq((int16_t)gc_read16(c->mem, ea), scale);
        } else {
            const uint32_t p = gc_read32(c->mem, ea);
            v0 = dq((int16_t)(p >> 16), scale);
            v1 = dq((int16_t)(p & 0xFFFFu), scale);
        }
        break;
    default:                                /* tipos 1-3: sin definir, Dolphin pone 0 */
        v0 = v1 = 0.0;
        break;
    }
    set_both(c, rd, v0, v1);
}

/* psq_l, psq_lu, psq_st, psq_stu: PSE y LSQE, luego MSR[FP]. */
static int quantized_unavailable(gekko_t *c)
{
    const uint32_t hid2 = c->spr[SPR_HID2];
    if (!(hid2 & HID2_PSE) || !(hid2 & HID2_LSQE)) {
        gekko_exception(c, EXC_PROGRAM, c->pc, SRR1_PROGRAM_ILLEGAL);
        return 1;
    }
    return gekko_fp_unavailable(c);
}

/* Forma D: W en el bit 16, I en 17-19, desplazamiento de 12 bits con signo. */
#define PSQ_W(i)    (((i) >> 15) & 1u)
#define PSQ_I(i)    (((i) >> 12) & 7u)
#define PSQ_D(i)    ((uint32_t)((int32_t)((i) << 20) >> 20))
/* Forma X (opcode 4): W en el bit 21, I en 22-24. */
#define PSQ_WX(i)   (((i) >> 10) & 1u)
#define PSQ_IX(i)   (((i) >> 7) & 7u)

static void psq_l(gekko_t *c, uint32_t i)
{
    if (quantized_unavailable(c))
        return;
    const uint32_t ea = (RA(i) ? c->gpr[RA(i)] : 0) + PSQ_D(i);
    dequantize(c, ea, PSQ_I(i), FD(i), PSQ_W(i));
}

static void psq_lu(gekko_t *c, uint32_t i)
{
    if (quantized_unavailable(c))
        return;
    const uint32_t ea = c->gpr[RA(i)] + PSQ_D(i);
    dequantize(c, ea, PSQ_I(i), FD(i), PSQ_W(i));
    c->gpr[RA(i)] = ea;
}

static void psq_st(gekko_t *c, uint32_t i)
{
    if (quantized_unavailable(c))
        return;
    const uint32_t ea = (RA(i) ? c->gpr[RA(i)] : 0) + PSQ_D(i);
    quantize(c, ea, PSQ_I(i), FS(i), PSQ_W(i));
}

static void psq_stu(gekko_t *c, uint32_t i)
{
    if (quantized_unavailable(c))
        return;
    const uint32_t ea = c->gpr[RA(i)] + PSQ_D(i);
    quantize(c, ea, PSQ_I(i), FS(i), PSQ_W(i));
    c->gpr[RA(i)] = ea;
}

/* Las formas X no piden LSQE (solo PSE, revisado en gekko.c). */
static void psq_lx(gekko_t *c, uint32_t i)   { dequantize(c, ea_x(c, i), PSQ_IX(i), FD(i), PSQ_WX(i)); }
static void psq_stx(gekko_t *c, uint32_t i)  { quantize(c, ea_x(c, i), PSQ_IX(i), FS(i), PSQ_WX(i)); }

static void psq_lux(gekko_t *c, uint32_t i)
{
    const uint32_t ea = c->gpr[RA(i)] + c->gpr[RB(i)];
    dequantize(c, ea, PSQ_IX(i), FD(i), PSQ_WX(i));
    c->gpr[RA(i)] = ea;
}

static void psq_stux(gekko_t *c, uint32_t i)
{
    const uint32_t ea = c->gpr[RA(i)] + c->gpr[RB(i)];
    quantize(c, ea, PSQ_IX(i), FS(i), PSQ_WX(i));
    c->gpr[RA(i)] = ea;
}

/* ---- tabla del opcode 4 --------------------------------------------------------- */

typedef struct op_entry {
    uint32_t    xo;
    gekko_op_fn fn;
} op_entry_t;

static const op_entry_t xo5_ops[] = {        /* forma A: XO de 5 bits */
    { 10, ps_sum0 }, { 11, ps_sum1 }, { 12, ps_muls0 }, { 13, ps_muls1 },
    { 14, ps_madds0 }, { 15, ps_madds1 }, { 18, ps_div }, { 20, ps_sub },
    { 21, ps_add }, { 23, ps_sel }, { 24, ps_res }, { 25, ps_mul },
    { 26, ps_rsqrte }, { 28, ps_msub }, { 29, ps_madd }, { 30, ps_nmsub },
    { 31, ps_nmadd },
};

static const op_entry_t xo6_ops[] = {        /* psq_*x: XO de 6 bits */
    { 6, psq_lx }, { 7, psq_stx }, { 38, psq_lux }, { 39, psq_stux },
};

static const op_entry_t xo10_ops[] = {       /* forma X: XO de 10 bits */
    { 0, ps_cmpu0 }, { 32, ps_cmpo0 }, { 40, ps_neg }, { 136, ps_nabs },
    { 264, ps_abs }, { 64, ps_cmpu1 }, { 72, ps_mr }, { 96, ps_cmpo1 },
    { 528, ps_merge00 }, { 560, ps_merge01 }, { 592, ps_merge10 }, { 624, ps_merge11 },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

void gekko_register_ps(void)
{
    scales_init();
    /* Como Dolphin: las formas cortas se repiten en todos los valores de los
     * bits altos del XO de 10 bits (que ahí son registros, W o I). */
    for (uint32_t hi = 0; hi < 32; hi++)
        for (size_t k = 0; k < COUNT(xo5_ops); k++)
            gekko_op_4[hi << 5 | xo5_ops[k].xo] = xo5_ops[k].fn;
    for (uint32_t hi = 0; hi < 16; hi++)
        for (size_t k = 0; k < COUNT(xo6_ops); k++)
            gekko_op_4[hi << 6 | xo6_ops[k].xo] = xo6_ops[k].fn;
    for (size_t k = 0; k < COUNT(xo10_ops); k++)
        gekko_op_4[xo10_ops[k].xo] = xo10_ops[k].fn;

    gekko_op_primary[56] = psq_l;
    gekko_op_primary[57] = psq_lu;
    gekko_op_primary[60] = psq_st;
    gekko_op_primary[61] = psq_stu;
}
