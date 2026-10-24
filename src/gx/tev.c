/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2009 Dolphin Emulator Project
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
 * El TEV (ver tev.h): VideoBackends/Software/Tev.cpp de Dolphin cf5cb7ef.
 *
 * Los registros de entrada son campos de bits en Dolphin (a, b y c de 8 bits
 * sin signo, d de 11 con signo, las coordenadas de 24 con signo): al guardar
 * se recortan igual. La niebla usa pow y sqrt de double, como el binario de
 * Dolphin (Tev.cpp llama a ::pow y ::sqrt, no a las versiones float).
 *
 * Lo que en Dolphin es un PanicAlert (canal de color 2-4, modo de matriz
 * indirecta 3, envoltura 7, formato de Z 3) acá sigue el mismo camino sin el
 * aviso. Los contadores de rendimiento no se llevan: Dolphin los tiene
 * apagados por omisión (PerfQueriesEnable) y el PE da 0.
 */
#include <math.h>
#include <string.h>

#include "core/log.h"
#include "gx/gx.h"
#include "gx/raster.h"
#include "gx/tev.h"
#include "gx/x86.h"

enum {
    BP_GENMODE = 0x00, BP_INDMTX = 0x06, BP_TEVIND = 0x10, BP_TEXSCALE = 0x25, BP_IREF = 0x27,
    BP_TEVORDER = 0x28, BP_ZMODE = 0x40, BP_ZCONTROL = 0x43, BP_COMBINER = 0xC0,
    BP_FOGRANGE = 0xE8, BP_FOG_A = 0xEE, BP_FOG_BMAG = 0xEF, BP_FOG_BSHIFT = 0xF0, BP_FOG_C = 0xF1,
    BP_FOG_COLOR = 0xF2, BP_ALPHATEST = 0xF3, BP_ZTEX1 = 0xF4, BP_ZTEX2 = 0xF5, BP_KSEL = 0xF6,
    XF_VIEWPORT_WD = 0x101A,
};

/* Los valores fijos de KonstSel */
enum { V0 = 0, V1_8 = 32, V1_4 = 64, V3_8 = 96, V1_2 = 128, V5_8 = 159, V3_4 = 191, V7_8 = 223, V1 = 255 };

/* Componentes de un texel (TextureSampler) */
enum { RED_SMP = 0, GRN_SMP = 1, BLU_SMP = 2, ALP_SMP = 3 };

static inline uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

/* Un campo de n bits con signo */
static inline int32_t sbits(uint32_t v, unsigned at, unsigned n)
{
    return (int32_t)(v << (32 - at - n)) >> (32 - n);
}

/* La vuelta de 32 bits de x86 */
static inline int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t wmul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline int32_t shl(int32_t v, unsigned n) { return (int32_t)((uint32_t)v << n); }

static inline int32_t sext24(int32_t v)
{
    return (int32_t)((uint32_t)v << 8) >> 8;
}

static inline int16_t clamp255(int16_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static inline int16_t clamp1024(int16_t v)
{
    return v < -1024 ? -1024 : v > 1023 ? 1023 : v;
}

static inline float clampf(float v, float lo, float hi)
{
    const float m = (v < lo) ? lo : v;
    return (hi < m) ? hi : m;
}

/* ---- texturas ------------------------------------------------------------------------- */

void gx_tex_sample(gc_gx_t *gx, int32_t s, int32_t t, int32_t lod, int linear, uint32_t texmap,
                   uint8_t out[4])
{
    (void)s;
    (void)t;
    (void)lod;
    (void)linear;
    (void)texmap;
    if (!gx->tex_noted) {
        gx->tex_noted = 1;
        GC_LOGI("gx: el TEV pide una textura; las texturas llegan en el paso C4 (negro)");
    }
    memset(out, 0, 4);
}

/* ---- constantes ----------------------------------------------------------------------- */

void gx_tev_set_konst(gc_gx_t *gx, gx_tev_t *t)
{
    for (int i = 0; i < 4; i++) {
        t->konst[i][GX_RED] = gx->tev_kcolor[i][0];
        t->konst[i][GX_GRN] = gx->tev_kcolor[i][1];
        t->konst[i][GX_BLU] = gx->tev_kcolor[i][2];
        t->konst[i][GX_ALP] = gx->tev_kcolor[i][3];
    }
}

/* m_KonstLUT: a, r, g, b de cada KonstSel */
static void konst_lut(const gx_tev_t *t, uint32_t sel, int16_t *a, int16_t *r, int16_t *g, int16_t *b)
{
    static const int16_t fixed[8] = { V1, V7_8, V3_4, V5_8, V1_2, V3_8, V1_4, V1_8 };
    if (sel < 8) {
        *a = *r = *g = *b = fixed[sel];
    } else if (sel < 12) {
        *a = *r = *g = *b = V0;                 /* inválidos: el hardware da 0 */
    } else if (sel < 16) {
        const int16_t *k = t->konst[sel - 12];
        *a = V0;
        *r = k[GX_RED];
        *g = k[GX_GRN];
        *b = k[GX_BLU];
    } else {
        static const int comp[4] = { GX_RED, GX_GRN, GX_BLU, GX_ALP };
        const int16_t v = t->konst[(sel - 16) & 3][comp[(sel - 16) >> 2]];
        *a = *r = *g = *b = v;
    }
}

/* GetSwapTable: qué componente del texel o del color va a r, g, b y a */
static void swap_table(const gc_gx_t *gx, uint32_t id, uint32_t swap[4])
{
    const uint32_t rg = gx->bp[BP_KSEL + 2 * id], ba = gx->bp[BP_KSEL + 2 * id + 1];
    swap[0] = bits(rg, 0, 2);
    swap[1] = bits(rg, 2, 2);
    swap[2] = bits(ba, 0, 2);
    swap[3] = bits(ba, 2, 2);
}

static void set_ras_color(const gc_gx_t *gx, gx_tev_t *t, uint32_t chan, uint32_t swaptable)
{
    if (chan == 0 || chan == 1) {
        const uint8_t *c = t->color[chan];
        uint32_t sw[4];
        swap_table(gx, swaptable, sw);
        t->ras[GX_RED] = c[sw[0]];
        t->ras[GX_GRN] = c[sw[1]];
        t->ras[GX_BLU] = c[sw[2]];
        t->ras[GX_ALP] = c[sw[3]];
        return;
    }
    int16_t v = 0;
    if (chan == 5)
        v = t->alpha_bump;
    else if (chan == 6)
        v = (uint8_t)(t->alpha_bump | t->alpha_bump >> 5);
    t->ras[0] = t->ras[1] = t->ras[2] = t->ras[3] = v;
}

/* ---- combinadores --------------------------------------------------------------------- */

/* InputRegType: a, b, c de 8 bits sin signo y d de 11 con signo */
typedef struct input {
    int32_t a, b, c, d;
} input_t;

static inline void set_input(input_t *in, int16_t a, int16_t b, int16_t c, int16_t d)
{
    in->a = (uint8_t)a;
    in->b = (uint8_t)b;
    in->c = (uint8_t)c;
    in->d = (int32_t)((uint32_t)d << 21) >> 21;
}

static const int16_t bias_lut[4] = { 0, 128, -128, 0 };
static const unsigned lshift_lut[4] = { 0, 1, 2, 0 };
static const unsigned rshift_lut[4] = { 0, 0, 0, 1 };

static void color_regular(gx_tev_t *t, uint32_t cc, const input_t in[4])
{
    const uint32_t scale = bits(cc, 20, 2), op = bits(cc, 18, 1), bias = bits(cc, 16, 2);
    const uint32_t dest = bits(cc, 22, 2);
    for (int i = GX_BLU; i <= GX_RED; i++) {
        const input_t *r = &in[i];
        const int32_t c = r->c + (r->c >> 7);
        int32_t temp = r->a * (256 - c) + r->b * c;
        temp = shl(temp, lshift_lut[scale]);
        temp += (scale == 3) ? 0 : op ? 127 : 128;
        temp >>= 8;
        temp = op ? -temp : temp;
        int32_t result = wadd(shl(r->d + bias_lut[bias], lshift_lut[scale]), temp);
        result >>= rshift_lut[scale];
        t->reg[dest][i] = (int16_t)result;
    }
}

/* El valor de 8, 16 o 24 bits que se compara */
static void compare_values(uint32_t mode, int alpha, const input_t in[4], int i, uint32_t *a, uint32_t *b)
{
    switch (mode) {
    case 0:                                     /* R8 */
        *a = (uint32_t)in[GX_RED].a;
        *b = (uint32_t)in[GX_RED].b;
        return;
    case 1:                                     /* GR16 */
        *a = (uint32_t)(in[GX_GRN].a << 8 | in[GX_RED].a);
        *b = (uint32_t)(in[GX_GRN].b << 8 | in[GX_RED].b);
        return;
    case 2:                                     /* BGR24 */
        *a = (uint32_t)(in[GX_BLU].a << 16 | in[GX_GRN].a << 8 | in[GX_RED].a);
        *b = (uint32_t)(in[GX_BLU].b << 16 | in[GX_GRN].b << 8 | in[GX_RED].b);
        return;
    default:                                    /* RGB8 o A8: el mismo componente */
        *a = (uint32_t)in[alpha ? GX_ALP : i].a;
        *b = (uint32_t)in[alpha ? GX_ALP : i].b;
        return;
    }
}

static void color_compare(gx_tev_t *t, uint32_t cc, const input_t in[4])
{
    const uint32_t mode = bits(cc, 20, 2), gt = !bits(cc, 18, 1), dest = bits(cc, 22, 2);
    for (int i = GX_BLU; i <= GX_RED; i++) {
        uint32_t a, b;
        compare_values(mode, 0, in, i, &a, &b);
        const int pass = gt ? a > b : a == b;
        t->reg[dest][i] = (int16_t)(in[i].d + (pass ? in[i].c : 0));
    }
}

static void alpha_regular(gx_tev_t *t, uint32_t ac, const input_t in[4])
{
    const uint32_t scale = bits(ac, 20, 2), op = bits(ac, 18, 1), bias = bits(ac, 16, 2);
    const input_t *r = &in[GX_ALP];
    const int32_t c = r->c + (r->c >> 7);
    int32_t temp = r->a * (256 - c) + r->b * c;
    temp = shl(temp, lshift_lut[scale]);
    temp += (scale == 3) ? 0 : op ? 127 : 128;
    temp = op ? (-temp >> 8) : (temp >> 8);
    int32_t result = wadd(shl(r->d + bias_lut[bias], lshift_lut[scale]), temp);
    result >>= rshift_lut[scale];
    t->reg[bits(ac, 22, 2)][GX_ALP] = (int16_t)result;
}

static void alpha_compare(gx_tev_t *t, uint32_t ac, const input_t in[4])
{
    uint32_t a, b;
    compare_values(bits(ac, 20, 2), 1, in, GX_ALP, &a, &b);
    const int pass = !bits(ac, 18, 1) ? a > b : a == b;
    t->reg[bits(ac, 22, 2)][GX_ALP] = (int16_t)(in[GX_ALP].d + (pass ? in[GX_ALP].c : 0));
}

/* m_ColorInputLUT: r, g, b de cada TevColorArg */
static void color_arg(const gx_tev_t *t, uint32_t arg, int16_t out[4])
{
    const int16_t *src = NULL;
    int16_t all = 0;
    switch (arg) {
    case 0: case 2: case 4: case 6:             /* prev, c0, c1, c2 .rgb */
        src = t->reg[arg >> 1];
        break;
    case 1: case 3: case 5: case 7:             /* .aaa */
        all = t->reg[arg >> 1][GX_ALP];
        break;
    case 8: src = t->tex; break;
    case 9: all = t->tex[GX_ALP]; break;
    case 10: src = t->ras; break;
    case 11: all = t->ras[GX_ALP]; break;
    case 12: all = V1; break;
    case 13: all = V1_2; break;
    case 14: src = t->stage_konst; break;
    default: all = V0; break;
    }
    if (src) {
        out[GX_RED] = src[GX_RED];
        out[GX_GRN] = src[GX_GRN];
        out[GX_BLU] = src[GX_BLU];
    } else {
        out[GX_RED] = out[GX_GRN] = out[GX_BLU] = all;
    }
}

/* m_AlphaInputLUT */
static int16_t alpha_arg(const gx_tev_t *t, uint32_t arg)
{
    switch (arg) {
    case 0: case 1: case 2: case 3: return t->reg[arg][GX_ALP];
    case 4: return t->tex[GX_ALP];
    case 5: return t->ras[GX_ALP];
    case 6: return t->stage_konst[GX_ALP];
    default: return V0;
    }
}

/* ---- etapas indirectas ---------------------------------------------------------------- */

static int32_t wrap_coord(int32_t coord, uint32_t mode)
{
    switch (mode) {
    case 0: return coord;
    case 1: return coord & ((256 << 7) - 1);
    case 2: return coord & ((128 << 7) - 1);
    case 3: return coord & ((64 << 7) - 1);
    case 4: return coord & ((32 << 7) - 1);
    case 5: return coord & ((16 << 7) - 1);
    default: return 0;                          /* ITW_0 (y el 7, inválido) */
    }
}

static void indirect(const gc_gx_t *gx, gx_tev_t *t, uint32_t stage, int32_t s, int32_t tc)
{
    const uint32_t ind = gx->bp[BP_TEVIND + stage];
    const uint8_t *map = t->ind_tex[bits(ind, 0, 2)];
    const uint32_t fmt = bits(ind, 2, 2);

    switch (bits(ind, 7, 2)) {                  /* bump alpha */
    case 0: t->alpha_bump = 0; break;
    case 1: t->alpha_bump = map[ALP_SMP]; break;
    case 2: t->alpha_bump = map[BLU_SMP]; break;
    default: t->alpha_bump = map[GRN_SMP]; break;
    }

    const int16_t bv = fmt == 0 ? -128 : 1;
    const int32_t bias[3] = { bits(ind, 4, 1) ? bv : 0, bits(ind, 5, 1) ? bv : 0, bits(ind, 6, 1) ? bv : 0 };
    static const unsigned fmt_shift[4] = { 0, 3, 4, 5 };
    const unsigned fs = fmt_shift[fmt];
    const int32_t ic[3] = { (map[ALP_SMP] >> fs) + bias[0], (map[BLU_SMP] >> fs) + bias[1],
                            (map[GRN_SMP] >> fs) + bias[2] };
    switch (fmt) {
    case 0: t->alpha_bump &= 0xf8; break;
    case 1: t->alpha_bump = (uint8_t)(t->alpha_bump << 5); break;
    case 2: t->alpha_bump = (uint8_t)(t->alpha_bump << 4); break;
    default: t->alpha_bump = (uint8_t)(t->alpha_bump << 3); break;
    }

    int32_t trans[2] = { 0, 0 };
    const uint32_t mtx_index = bits(ind, 9, 2), mtx_id = bits(ind, 11, 2);
    if (mtx_index != 0) {
        const uint32_t *m = &gx->bp[BP_INDMTX + 3 * (mtx_index - 1)];
        const int32_t scale = (int32_t)(bits(m[0], 22, 2) | bits(m[1], 22, 2) << 2 | bits(m[2], 22, 1) << 4);
        const int32_t shift = 17 - scale;
        switch (mtx_id) {
        case 0:                                 /* S0.10 por s17.7: dividido por 8 */
            trans[0] = wadd(wadd(wmul(sbits(m[0], 0, 11), ic[0]), wmul(sbits(m[1], 0, 11), ic[1])),
                            wmul(sbits(m[2], 0, 11), ic[2])) >> 3;
            trans[1] = wadd(wadd(wmul(sbits(m[0], 11, 11), ic[0]), wmul(sbits(m[1], 11, 11), ic[1])),
                            wmul(sbits(m[2], 11, 11), ic[2])) >> 3;
            break;
        case 1:
            trans[0] = wmul(s, ic[0]) / 256;
            trans[1] = wmul(tc, ic[0]) / 256;
            break;
        case 2:
            trans[0] = wmul(s, ic[1]) / 256;
            trans[1] = wmul(tc, ic[1]) / 256;
            break;
        default:
            return;                             /* "Invalid indirect matrix ID": sale sin tocar TexCoord */
        }
        trans[0] = shift >= 0 ? trans[0] >> shift : shl(trans[0], (unsigned)-shift);
        trans[1] = shift >= 0 ? trans[1] >> shift : shl(trans[1], (unsigned)-shift);
    }

    const int32_t ns = wadd(wrap_coord(s, bits(ind, 13, 3)), trans[0]);
    const int32_t nt = wadd(wrap_coord(tc, bits(ind, 16, 3)), trans[1]);
    if (bits(ind, 20, 1)) {                     /* fb_addprev */
        t->tc_s = sext24(wadd(t->tc_s, ns));
        t->tc_t = sext24(wadd(t->tc_t, nt));
    } else {
        t->tc_s = sext24(ns);
        t->tc_t = sext24(nt);
    }
}

/* ---- prueba de alfa y niebla ---------------------------------------------------------- */

static int alpha_compare_op(int alpha, int ref, uint32_t comp)
{
    switch (comp) {
    case 0: return 0;
    case 1: return alpha < ref;
    case 2: return alpha == ref;
    case 3: return alpha <= ref;
    case 4: return alpha > ref;
    case 5: return alpha != ref;
    case 6: return alpha >= ref;
    default: return 1;
    }
}

static int alpha_test(const gc_gx_t *gx, int alpha)
{
    const uint32_t at = gx->bp[BP_ALPHATEST];
    const int c0 = alpha_compare_op(alpha, (int)bits(at, 0, 8), bits(at, 16, 3));
    const int c1 = alpha_compare_op(alpha, (int)bits(at, 8, 8), bits(at, 19, 3));
    switch (bits(at, 22, 2)) {
    case 0: return c0 && c1;
    case 1: return c0 || c1;
    case 2: return c0 ^ c1;
    default: return !(c0 ^ c1);
    }
}

/* FogParam0/3::FloatValue: la mantisa de 11 bits pasa a 23 */
static float fog_float(uint32_t v)
{
    const uint32_t u = bits(v, 19, 1) << 31 | bits(v, 11, 8) << 23 | bits(v, 0, 11) << 12;
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* Con a y c infinitos o NaN el hardware da A = 0 y C = ±inf (IsNaNCase) */
static int fog_nan_case(const gc_gx_t *gx)
{
    return bits(gx->bp[BP_FOG_A], 11, 8) == 255 && bits(gx->bp[BP_FOG_C], 11, 8) == 255;
}

static float fog_a(const gc_gx_t *gx)
{
    return fog_nan_case(gx) ? 0.0f : fog_float(gx->bp[BP_FOG_A]);
}

static float fog_c(const gc_gx_t *gx)
{
    if (fog_nan_case(gx))
        return !bits(gx->bp[BP_FOG_A], 19, 1) && !bits(gx->bp[BP_FOG_C], 19, 1) ? -INFINITY : INFINITY;
    return fog_float(gx->bp[BP_FOG_C]);
}

/* FogRangeKElement::GetValue; K[5] cae en el registro siguiente (fog.a), como en Dolphin */
static float fog_k(const gc_gx_t *gx, int32_t index)
{
    const uint32_t v = gx->bp[BP_FOGRANGE + 1 + (uint32_t)(index / 2)];
    return (float)((index % 2) ? bits(v, 0, 12) : bits(v, 12, 12)) / 256.f;
}

static void fog(const gc_gx_t *gx, const gx_tev_t *t, uint8_t out[4])
{
    const uint32_t c = gx->bp[BP_FOG_C];
    const uint32_t fsel = bits(c, 21, 3);
    if (fsel == 0)
        return;

    float ze;
    if (!bits(c, 20, 1)) {                      /* perspectiva: ze = A / (B - (Zs >> B_SHF)) */
        const int32_t zs = t->pos[2] >> (gx->bp[BP_FOG_BSHIFT] & 31u);
        const int32_t denom = (int32_t)(gx->bp[BP_FOG_BMAG] - (uint32_t)zs);
        ze = (fog_a(gx) * 16777215.0f) / (float)denom;
    } else {                                    /* ortográfica: ze = A × Zs */
        ze = fog_a(gx) * ((float)t->pos[2] / 16777215.0f);
    }

    const uint32_t range = gx->bp[BP_FOGRANGE];
    if (bits(range, 10, 1)) {
        float wd;
        memcpy(&wd, &gx->xf[XF_VIEWPORT_WD], sizeof wd);
        const float offset = (float)(t->pos[0] - ((int32_t)bits(range, 0, 10) - 342)) / wd;
        float fi = 9.f - fabsf(offset) * 9.f;
        fi = clampf(fi, 0.f, 9.f);
        int32_t lower = gx_x86_f2i(fi);
        if (lower < 0 || lower > 9)
            lower = 0;                          /* NaN: en Dolphin lee fuera de bpmem */
        const int32_t upper = lower + 1;
        const float kl = fog_k(gx, lower) * 4.f;
        const float ku = fog_k(gx, upper) * 4.f;
        const float factor = (float)upper - fi;
        const float k = kl * factor + ku * (1.f - factor);
        const float x_adjust = (float)(sqrt((double)(offset * offset + k * k)) / (double)k);
        ze *= x_adjust;
    }

    ze -= fog_c(gx);
    float f = clampf(ze, 0.f, 1.f);
    switch (fsel) {
    case 4:                                     /* Exp */
        f = (float)(1.0 - pow(2.0, (double)(-8.0f * f)));
        break;
    case 5:                                     /* ExpSq */
        f = (float)(1.0 - pow(2.0, (double)(-8.0f * f * f)));
        break;
    case 6:                                     /* BackwardsExp */
        f = 1.0f - f;
        f = (float)pow(2.0, (double)(-8.0f * f));
        break;
    case 7:                                     /* BackwardsExpSq */
        f = 1.0f - f;
        f = (float)pow(2.0, (double)(-8.0f * f * f));
        break;
    default:                                    /* lineal (y 1 y 3) */
        break;
    }

    const uint32_t fi = gx_x86_f2u(f * 256);
    const uint32_t inv = 256 - fi;
    const uint32_t col = gx->bp[BP_FOG_COLOR];
    out[GX_RED] = (uint8_t)((out[GX_RED] * inv + fi * bits(col, 16, 8)) >> 8);
    out[GX_GRN] = (uint8_t)((out[GX_GRN] * inv + fi * bits(col, 8, 8)) >> 8);
    out[GX_BLU] = (uint8_t)((out[GX_BLU] * inv + fi * bits(col, 0, 8)) >> 8);
}

/* ---- el píxel ------------------------------------------------------------------------- */

void gx_tev_draw(gc_gx_t *gx, gx_tev_t *t)
{
    const uint32_t *bp = gx->bp;
    const uint32_t genmode = bp[BP_GENMODE];
    const uint32_t ntexgens = bits(genmode, 0, 4);
    const uint32_t nstages = bits(genmode, 10, 4);
    uint32_t nind = bits(genmode, 16, 3);
    if (nind > 4)
        nind = 4;

    for (int i = 0; i < 4; i++) {
        t->reg[i][GX_RED] = gx->tev_color[i][0];
        t->reg[i][GX_GRN] = gx->tev_color[i][1];
        t->reg[i][GX_BLU] = gx->tev_color[i][2];
        t->reg[i][GX_ALP] = gx->tev_color[i][3];
    }

    /* Etapas indirectas */
    const uint32_t iref = bp[BP_IREF];
    for (uint32_t s = 0; s < nind; s++) {
        uint32_t sel = bits(iref, 6 * s + 3, 3);
        const uint32_t texmap = bits(iref, 6 * s, 3);
        /* Una coordenada que no existe es la 0 (bug 11462 de Dolphin) */
        if (sel >= ntexgens)
            sel = 0;
        const uint32_t ts = bp[BP_TEXSCALE + (s >> 1)];
        const unsigned ss = (s & 1u) ? bits(ts, 8, 4) : bits(ts, 0, 4);
        const unsigned st = (s & 1u) ? bits(ts, 12, 4) : bits(ts, 4, 4);
        gx_tex_sample(gx, t->uv[sel][0] >> ss, t->uv[sel][1] >> st, t->ind_lod[s], t->ind_linear[s],
                      texmap, t->ind_tex[s]);
    }

    for (uint32_t s = 0; s <= nstages; s++) {
        const uint32_t order = bp[BP_TEVORDER + (s >> 1)];
        const unsigned sh = (s & 1u) ? 12 : 0;
        const uint32_t cc = bp[BP_COMBINER + 2 * s], ac = bp[BP_COMBINER + 2 * s + 1];

        uint32_t sel = bits(order, sh + 3, 3);
        const uint32_t texmap = bits(order, sh, 3);
        if (sel >= ntexgens)
            sel = 0;

        indirect(gx, t, s, t->uv[sel][0], t->uv[sel][1]);

        if (bits(order, sh + 6, 1)) {
            uint8_t texel[4];
            if (ntexgens > 0)
                gx_tex_sample(gx, t->tc_s, t->tc_t, t->tex_lod[s], t->tex_linear[s], texmap, texel);
            else
                memset(texel, 0, 4);            /* sin coordenadas: negro */
            t->raw_tex[GX_RED] = texel[0];
            t->raw_tex[GX_GRN] = texel[1];
            t->raw_tex[GX_BLU] = texel[2];
            t->raw_tex[GX_ALP] = texel[3];
            uint32_t sw[4];
            swap_table(gx, bits(ac, 2, 2), sw);
            t->tex[GX_RED] = texel[sw[0]];
            t->tex[GX_GRN] = texel[sw[1]];
            t->tex[GX_BLU] = texel[sw[2]];
            t->tex[GX_ALP] = texel[sw[3]];
        }

        /* Konst de la etapa */
        const uint32_t ksel = bp[BP_KSEL + (s >> 1)];
        const uint32_t kc = (s & 1u) ? bits(ksel, 14, 5) : bits(ksel, 4, 5);
        const uint32_t ka = (s & 1u) ? bits(ksel, 19, 5) : bits(ksel, 9, 5);
        int16_t a, r, g, b;
        konst_lut(t, kc, &a, &r, &g, &b);
        t->stage_konst[GX_RED] = r;
        t->stage_konst[GX_GRN] = g;
        t->stage_konst[GX_BLU] = b;
        konst_lut(t, ka, &a, &r, &g, &b);
        t->stage_konst[GX_ALP] = a;

        set_ras_color(gx, t, bits(order, sh + 7, 3), bits(ac, 0, 2));

        /* Entradas */
        input_t in[4];
        int16_t ca[4], cb[4], ccv[4], cd[4];
        color_arg(t, bits(cc, 12, 4), ca);
        color_arg(t, bits(cc, 8, 4), cb);
        color_arg(t, bits(cc, 4, 4), ccv);
        color_arg(t, bits(cc, 0, 4), cd);
        for (int i = GX_BLU; i <= GX_RED; i++)
            set_input(&in[i], ca[i], cb[i], ccv[i], cd[i]);
        set_input(&in[GX_ALP], alpha_arg(t, bits(ac, 13, 3)), alpha_arg(t, bits(ac, 10, 3)),
                  alpha_arg(t, bits(ac, 7, 3)), alpha_arg(t, bits(ac, 4, 3)));

        if (bits(cc, 16, 2) != 3)
            color_regular(t, cc, in);
        else
            color_compare(t, cc, in);
        int16_t *cdst = t->reg[bits(cc, 22, 2)];
        for (int i = GX_BLU; i <= GX_RED; i++)
            cdst[i] = bits(cc, 19, 1) ? clamp255(cdst[i]) : clamp1024(cdst[i]);

        if (bits(ac, 16, 2) != 3)
            alpha_regular(t, ac, in);
        else
            alpha_compare(t, ac, in);
        int16_t *adst = &t->reg[bits(ac, 22, 2)][GX_ALP];
        *adst = bits(ac, 19, 1) ? clamp255(*adst) : clamp1024(*adst);
    }

    /* Lo de la última etapa va a la pantalla, sea cual sea su destino */
    const uint32_t cdest = bits(bp[BP_COMBINER + 2 * nstages], 22, 2);
    const uint32_t adest = bits(bp[BP_COMBINER + 2 * nstages + 1], 22, 2);
    uint8_t out[4];
    out[GX_ALP] = (uint8_t)t->reg[adest][GX_ALP];
    out[GX_BLU] = (uint8_t)t->reg[cdest][GX_BLU];
    out[GX_GRN] = (uint8_t)t->reg[cdest][GX_GRN];
    out[GX_RED] = (uint8_t)t->reg[cdest][GX_RED];

    if (!alpha_test(gx, out[GX_ALP]))
        return;

    /* Textura de Z */
    const uint32_t ztex2 = bp[BP_ZTEX2];
    if (bits(ztex2, 2, 2) != 0) {
        uint32_t ztex = bits(bp[BP_ZTEX1], 0, 24);
        switch (bits(ztex2, 0, 2)) {
        case 0:
            ztex += (uint32_t)t->raw_tex[GX_ALP];
            break;
        case 1:
            ztex += (uint32_t)(t->raw_tex[GX_ALP] << 8 | t->raw_tex[GX_RED]);
            break;
        case 2:
            ztex += (uint32_t)(t->raw_tex[GX_RED] << 16 | t->raw_tex[GX_GRN] << 8 | t->raw_tex[GX_BLU]);
            break;
        default:
            break;                              /* "Invalid ztex format": queda el bias */
        }
        if (bits(ztex2, 2, 2) == 1)
            ztex += (uint32_t)t->pos[2];
        t->pos[2] = (int32_t)(ztex & 0x00FFFFFFu);
    }

    fog(gx, t, out);

    /* Z tardía (GetEmulatedZ() == Late) */
    if (bits(bp[BP_ZMODE], 0, 1) && !bits(bp[BP_ZCONTROL], 6, 1)) {
        if (!gx_efb_zcompare(gx->efb, bp, (uint32_t)t->pos[0], (uint32_t)t->pos[1], (uint32_t)t->pos[2]))
            return;
    }

    /* La GPU rasteriza de a 2 × 2: la caja se redondea a esos cuadros */
    gc_gx_bbox_update(gx, (uint32_t)t->pos[0], (uint32_t)t->pos[1]);

    const uint32_t rgba = (uint32_t)out[GX_RED] << 24 | (uint32_t)out[GX_GRN] << 16 |
                          (uint32_t)out[GX_BLU] << 8 | out[GX_ALP];
    gx_efb_blend(gx->efb, bp, (uint32_t)t->pos[0], (uint32_t)t->pos[1], rgba);
}
