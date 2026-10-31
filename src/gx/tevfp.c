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
 * El TEV como fragment program (ver tevfp.h). Sigue a tev.c paso por paso:
 *
 *   - Las cuentas enteras de Dolphin van en float con valores enteros (menos
 *     de 2^24: exactas). x >> n es FLR(x × 2^-n) (también con negativos, como
 *     el corrimiento aritmético); la división entera de C (hacia 0) es
 *     FLR(|x|) con el signo; (uint8_t) x es x - 256 × FLR(x / 256).
 *   - Una constante por instrucción: cuando hacen falta dos valores van en la
 *     misma con swizzles distintos.
 *   - Los registros del TEV, la textura, el color rasterizado y la constante
 *     de la etapa son vectores r, g, b, a: el color y el alfa de una etapa se
 *     calculan juntos, con constantes por componente cuando difieren.
 *   - Lo que en Dolphin queda de otro píxel no se puede repetir: se marca en
 *     stale y las pruebas no lo comparan.
 *   - Sin sext24 de las coordenadas (pasa de ±2^23 recién con texturas de más
 *     de 65536 texeles) ni el caso q = 0 de la proyección.
 */
#include <math.h>
#include <string.h>

#include "gx/gx.h"
#include "gx/tev.h"
#include "gx/tevfp.h"

enum {
    BP_GENMODE = 0x00, BP_INDMTX = 0x06, BP_TEVIND = 0x10, BP_TEXSCALE = 0x25, BP_IREF = 0x27,
    BP_TEVORDER = 0x28, BP_SETIMAGE0 = 0x88, BP_SETIMAGE0_4 = 0xA8, BP_COMBINER = 0xC0,
    BP_FOGRANGE = 0xE8, BP_FOG_BMAG = 0xEF, BP_FOG_BSHIFT = 0xF0, BP_FOG_C = 0xF1, BP_FOG_COLOR = 0xF2,
    BP_ALPHATEST = 0xF3, BP_ZTEX1 = 0xF4, BP_ZTEX2 = 0xF5, BP_KSEL = 0xF6,
    XF_VIEWPORT_WD = 0x101A,
};

/* Registros R del programa */
enum {
    R_OUT = 0, R_DEPTH = 1, R_REG = 2,          /* 2-5: PREV, C0, C1, C2 */
    R_TEX = 6, R_RAW = 7, R_RAS = 8, R_KON = 9, R_COL = 10, /* 10-11 */
    R_UV = 12,                                  /* 12-19 */
    R_TC = 20, R_IND = 21,                      /* 21-24 */
    R_BUMP = 25, R_A = 26, R_B = 27, R_C = 28, R_D = 29, R_RES = 30,
    T0 = 31, T1 = 32, T2 = 33, T3 = 34, R_OUTI = 35, R_Z = 36,
};

#define MX   1u
#define MY   2u
#define MZ   4u
#define MW   8u
#define MXY  3u
#define MXYZ 7u
#define MALL 15u

#define SW(x, y, z, w) RSXFP_SWZ(x, y, z, w)
#define SP(c)          RSXFP_SPLAT(c)
#define XYZW           RSXFP_XYZW

static inline uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

static inline int32_t sbits(uint32_t v, unsigned at, unsigned n)
{
    return (int32_t)(v << (32 - at - n)) >> (32 - n);
}

/* ---- clave ---------------------------------------------------------------------------- */

void gx_tevfp_key(const uint32_t bp[256], gx_tevfp_key_t *key)
{
    uint32_t n = 0;
    uint32_t *v = key->v;
    v[n++] = bp[BP_GENMODE] & 0x73C7Fu;         /* coordenadas, canales, etapas, indirectas */
    for (uint32_t r = BP_TEVIND; r < BP_TEVIND + 16; r++)
        v[n++] = bp[r] & 0x1FFFFFu;
    v[n++] = bp[BP_TEXSCALE] & 0xFFFFu;
    v[n++] = bp[BP_TEXSCALE + 1] & 0xFFFFu;
    v[n++] = bp[BP_IREF] & 0xFFFFFFu;
    for (uint32_t r = BP_TEVORDER; r < BP_TEVORDER + 8; r++)
        v[n++] = bp[r] & 0xFFFFFFu;
    for (uint32_t r = BP_COMBINER; r < BP_COMBINER + 32; r++)
        v[n++] = bp[r] & 0xFFFFFFu;
    v[n++] = bp[BP_ALPHATEST] & 0xFF0000u;      /* comparaciones y lógica, sin las referencias */
    v[n++] = bp[BP_ZTEX2] & 0xFu;
    for (uint32_t r = BP_KSEL; r < BP_KSEL + 8; r++)
        v[n++] = bp[r] & 0xFFFFFFu;
    v[n++] = (bp[BP_FOG_C] & 0xF00000u) | (bp[BP_FOGRANGE] & 0x400u);   /* niebla: proyección, tipo, rango */
    uint32_t scales = 0;                        /* la escala de las 3 matrices indirectas: el corrimiento */
    for (uint32_t m = 0; m < 3; m++) {
        const uint32_t *r = &bp[BP_INDMTX + 3 * m];
        scales |= (bits(r[0], 22, 2) | bits(r[1], 22, 2) << 2 | bits(r[2], 22, 1) << 4) << (5 * m);
    }
    v[n++] = scales;
    while (n < GX_TEVFP_KEY)
        v[n++] = 0;
}

uint32_t gx_tevfp_key_hash(const gx_tevfp_key_t *key)
{
    uint32_t h = 2166136261u;                   /* FNV-1a */
    for (uint32_t i = 0; i < GX_TEVFP_KEY; i++)
        for (unsigned b = 0; b < 32; b += 8) {
            h ^= (key->v[i] >> b) & 0xFFu;
            h *= 16777619u;
        }
    return h;
}

/* ---- emisión -------------------------------------------------------------------------- */

typedef struct gen {
    rsxfp_t      fp;
    gx_tevfp_t  *p;
    const uint32_t *bp;
    int          err;
    uint32_t     ntex, ncol, nstages, nind;
    int          tex_defined, tc_defined;
} gen_t;

static const rsxfp_src_t NONE;
static const float K0[4];

static rsxfp_src_t R(uint8_t r) { return rsxfp_r(r, XYZW); }
static rsxfp_src_t RS(uint8_t r, uint8_t swz) { return rsxfp_r(r, swz); }
static rsxfp_src_t IN(uint8_t reg, uint8_t swz) { return rsxfp_in(reg, swz); }
static rsxfp_src_t K(uint8_t swz) { return rsxfp_k(swz); }
static rsxfp_src_t NEG(rsxfp_src_t s) { return rsxfp_neg(s); }
static rsxfp_src_t ABS(rsxfp_src_t s) { return rsxfp_abs(s); }

static int32_t ins_full(gen_t *g, uint8_t op, int8_t dst, uint8_t mask, rsxfp_src_t a, rsxfp_src_t b,
                        rsxfp_src_t c, const float *k, int sat, int cc_write)
{
    rsxfp_insn_t i;
    memset(&i, 0, sizeof i);
    i.op = op;
    i.dst = dst;
    i.mask = mask;
    i.sat = (uint8_t)sat;
    i.cc_write = (uint8_t)cc_write;
    i.cond = RSXFP_TR;
    i.cond_swz = XYZW;
    i.src[0] = a;
    i.src[1] = b;
    i.src[2] = c;
    if (k)
        memcpy(i.k, k, sizeof i.k);
    const int32_t at = rsxfp_emit(&g->fp, &i);
    if (at < -1 && !g->err)
        g->err = at;
    return at;
}

/* Sin constante */
static void ins(gen_t *g, uint8_t op, int8_t dst, uint8_t mask, rsxfp_src_t a, rsxfp_src_t b, rsxfp_src_t c)
{
    ins_full(g, op, dst, mask, a, b, c, NULL, 0, 0);
}

/* Con una constante fija */
static void insk(gen_t *g, uint8_t op, int8_t dst, uint8_t mask, rsxfp_src_t a, rsxfp_src_t b, rsxfp_src_t c,
                 float k0, float k1, float k2, float k3)
{
    const float k[4] = { k0, k1, k2, k3 };
    ins_full(g, op, dst, mask, a, b, c, k, 0, 0);
}

/* Con una constante que se escribe en cada dibujo */
static void insp(gen_t *g, uint8_t op, int8_t dst, uint8_t mask, rsxfp_src_t a, rsxfp_src_t b, rsxfp_src_t c,
                 uint8_t kind, uint8_t arg)
{
    const int32_t at = ins_full(g, op, dst, mask, a, b, c, K0, 0, 0);
    if (at < 0)
        return;
    gx_tevfp_t *p = g->p;
    if (p->npatch >= GX_TEVFP_PATCHES) {
        if (!g->err)
            g->err = RSXFP_ESPACE;
        return;
    }
    p->patch[p->npatch].at = (uint16_t)at;
    p->patch[p->npatch].kind = kind;
    p->patch[p->npatch].arg = arg;
    p->npatch++;
}

/* x = x - n × FLR(x / n) (n potencia de 2): el resto no negativo */
static void mod_n(gen_t *g, uint8_t r, uint8_t mask, float n)
{
    insk(g, RSXFP_MUL, T3, mask, R(r), K(SP(0)), NONE, 1.0f / n, -n, 0, 0);
    ins(g, RSXFP_FLR, T3, mask, R(T3), NONE, NONE);
    insk(g, RSXFP_MAD, (int8_t)r, mask, R(T3), K(SP(1)), R(r), 1.0f / n, -n, 0, 0);
}

/* dst = la parte entera de src hacia 0 (la división de C), con scratch */
static void trunc_to(gen_t *g, uint8_t dst, uint8_t src, uint8_t mask)
{
    insk(g, RSXFP_SGE, T3, mask, R(src), K(SP(0)), NONE, 0, 2, -1, 0);
    insk(g, RSXFP_MAD, T3, mask, R(T3), K(SP(1)), K(SP(2)), 0, 2, -1, 0);   /* signo: 1 o -1 */
    ins(g, RSXFP_FLR, (int8_t)dst, mask, ABS(R(src)), NONE, NONE);
    ins(g, RSXFP_MUL, (int8_t)dst, mask, R(dst), R(T3), NONE);
}

/* sext24 de Dolphin: el valor módulo 2^24, de -2^23 a 2^23 - 1 (exacto si
 * |x| < 2^24) */
static void sext24(gen_t *g, uint8_t r, uint8_t mask)
{
    mod_n(g, r, mask, 16777216.0f);
    insk(g, RSXFP_SGE, T3, mask, R(r), K(SP(0)), NONE, 8388608.0f, -16777216.0f, 0, 0);
    insk(g, RSXFP_MAD, (int8_t)r, mask, R(T3), K(SP(1)), R(r), 8388608.0f, -16777216.0f, 0, 0);
}

/* Un texel: TEX en unidad texmap con la coordenada de coord.xy (s17.7)
 * escalada; RGBA de 0 a 255 en dst */
static void sample(gen_t *g, uint8_t dst, uint8_t coord, uint8_t texmap, int ind, uint8_t stage)
{
    gx_tevfp_t *p = g->p;
    insp(g, RSXFP_MUL, T0, MXY, R(coord), K(XYZW), NONE, GX_TEVFP_P_TEXSCALE, texmap);
    rsxfp_insn_t i;
    memset(&i, 0, sizeof i);
    i.op = RSXFP_TEX;
    i.dst = T1;
    i.mask = MALL;
    i.unit = texmap;                            /* la unidad del RSX es el texmap de GX */
    i.cond = RSXFP_TR;
    i.cond_swz = XYZW;
    i.src[0] = R(T0);
    const int32_t r = rsxfp_emit(&g->fp, &i);
    if (r < -1 && !g->err)
        g->err = r;
    if (p->ntex < GX_TEVFP_TEX) {
        p->tex[p->ntex].ind = (uint8_t)ind;
        p->tex[p->ntex].stage = stage;
        p->tex[p->ntex].texmap = texmap;
        p->ntex++;
    } else if (!g->err) {
        g->err = RSXFP_ESPACE;
    }
    insk(g, RSXFP_MAD, T1, MALL, R(T1), K(SP(0)), K(SP(1)), 255.0f, 0.5f, 0, 0);
    ins(g, RSXFP_FLR, (int8_t)dst, MALL, R(T1), NONE, NONE);
}

/* ---- partes del programa -------------------------------------------------------------- */

/* Los colores rasterizados: (uint8_t) f2i(clamp(v, 0, 255)) */
static void load_colors(gen_t *g)
{
    for (uint32_t c = 0; c < g->ncol; c++) {
        const uint8_t r = (uint8_t)(R_COL + c);
        for (uint32_t k = 0; k < 4; k++)
            ins(g, RSXFP_MOV, (int8_t)r, (uint8_t)(1u << k), IN((uint8_t)(RSXFP_TC0 + 4 * c + k), SP(3)), NONE,
                NONE);
        insk(g, RSXFP_MAX, (int8_t)r, MALL, R(r), K(SP(0)), NONE, 0, 255, 0, 0);
        insk(g, RSXFP_MIN, (int8_t)r, MALL, R(r), K(SP(1)), NONE, 0, 255, 0, 0);
        ins(g, RSXFP_FLR, (int8_t)r, MALL, R(r), NONE, NONE);
    }
}

/* La coordenada i en s17.7: f2i(S / Q × 128) */
static void load_uv(gen_t *g, uint32_t i)
{
    const uint8_t uv = (uint8_t)(R_UV + i);
    ins(g, RSXFP_MOV, T0, MXYZ, IN((uint8_t)(RSXFP_TC0 + i), XYZW), NONE, NONE);
    ins(g, RSXFP_RCP, T1, MX, RS(T0, SP(2)), NONE, NONE);
    ins(g, RSXFP_MUL, T0, MXY, R(T0), RS(T1, SP(0)), NONE);
    insk(g, RSXFP_MUL, T0, MXY, R(T0), K(SP(0)), NONE, 128, 0, 0, 0);
    trunc_to(g, uv, T0, MXY);
}

/* Las etapas indirectas: el texel de cada una en R_IND */
static void indirect_lookups(gen_t *g)
{
    const uint32_t iref = g->bp[BP_IREF];
    for (uint32_t s = 0; s < g->nind; s++) {
        uint32_t sel = bits(iref, 6 * s + 3, 3);
        const uint32_t texmap = bits(iref, 6 * s, 3);
        if (sel >= g->ntex)
            sel = 0;
        const uint8_t dst = (uint8_t)(R_IND + s);
        if (g->ntex == 0) {
            g->p->stale |= GX_TEVFP_STALE_IND;  /* coordenadas de otro píxel */
            insk(g, RSXFP_MOV, (int8_t)dst, MALL, K(XYZW), NONE, NONE, 0, 0, 0, 0);
            continue;
        }
        const uint32_t ts = g->bp[BP_TEXSCALE + (s >> 1)];
        const unsigned ss = (s & 1u) ? bits(ts, 8, 4) : bits(ts, 0, 4);
        const unsigned st = (s & 1u) ? bits(ts, 12, 4) : bits(ts, 4, 4);
        insk(g, RSXFP_MUL, T2, MXY, R((uint8_t)(R_UV + sel)), K(XYZW), NONE, ldexpf(1.0f, -(int)ss),
             ldexpf(1.0f, -(int)st), 0, 0);
        ins(g, RSXFP_FLR, T2, MXY, R(T2), NONE, NONE);
        sample(g, dst, T2, (uint8_t)texmap, 1, (uint8_t)s);
    }
}

/* Tev::Indirect de la etapa s: tc (R_TC) y el alfa de bump (R_BUMP.x) */
static void stage_indirect(gen_t *g, uint32_t s, uint32_t sel)
{
    const uint32_t ind = g->bp[BP_TEVIND + s];
    const uint8_t uv = (uint8_t)(R_UV + sel);
    if (ind == 0) {                             /* lo común: tc = la coordenada */
        insk(g, RSXFP_MOV, R_BUMP, MX, K(SP(0)), NONE, NONE, 0, 0, 0, 0);
        if (g->ntex > 0) {
            ins(g, RSXFP_MOV, R_TC, MXY, R(uv), NONE, NONE);
            g->tc_defined = 1;
        }
        return;
    }

    const uint32_t bt = bits(ind, 0, 2), fmt = bits(ind, 2, 2), bs = bits(ind, 7, 2);
    const uint32_t mtx = bits(ind, 9, 2), mid = bits(ind, 11, 2);
    const uint32_t sw = bits(ind, 13, 3), tw = bits(ind, 16, 3), addprev = bits(ind, 20, 1);
    const uint8_t map = (uint8_t)(R_IND + bt);
    if (bt >= g->nind && (bs != 0 || mtx != 0))
        g->p->stale |= GX_TEVFP_STALE_IND;

    /* Alfa de bump */
    static const uint8_t bump_comp[4] = { 0, 3, 2, 1 };    /* -, a, b, g */
    if (bs == 0)
        insk(g, RSXFP_MOV, R_BUMP, MX, K(SP(0)), NONE, NONE, 0, 0, 0, 0);
    else
        ins(g, RSXFP_MOV, R_BUMP, MX, RS(map, SP(bump_comp[bs])), NONE, NONE);

    /* ic = (a, b, g) >> fs + sesgo, en T0.xyz */
    static const int fmt_shift[4] = { 0, 3, 4, 5 };
    const int fs = fmt_shift[fmt];
    if (fs == 0) {
        ins(g, RSXFP_MOV, T0, MXYZ, RS(map, SW(3, 2, 1, 1)), NONE, NONE);
    } else {
        insk(g, RSXFP_MUL, T0, MXYZ, RS(map, SW(3, 2, 1, 1)), K(SP(0)), NONE, ldexpf(1.0f, -fs), 0, 0, 0);
        ins(g, RSXFP_FLR, T0, MXYZ, R(T0), NONE, NONE);
    }
    const float bv = fmt == 0 ? -128.0f : 1.0f;
    if (bits(ind, 4, 3))
        insk(g, RSXFP_ADD, T0, MXYZ, R(T0), K(XYZW), NONE, bits(ind, 4, 1) ? bv : 0, bits(ind, 5, 1) ? bv : 0,
             bits(ind, 6, 1) ? bv : 0, 0);

    /* El formato corre el alfa de bump */
    if (bs != 0 || fmt != 0) {
        if (fmt == 0) {
            insk(g, RSXFP_MUL, T1, MX, RS(R_BUMP, SP(0)), K(SP(0)), NONE, 0.125f, 8, 0, 0);
            ins(g, RSXFP_FLR, T1, MX, R(T1), NONE, NONE);
            insk(g, RSXFP_MUL, R_BUMP, MX, RS(T1, SP(0)), K(SP(1)), NONE, 0.125f, 8, 0, 0);
        } else {
            insk(g, RSXFP_MUL, R_BUMP, MX, RS(R_BUMP, SP(0)), K(SP(0)), NONE, (float)(1 << (6 - fmt)), 0, 0, 0);
            mod_n(g, R_BUMP, MX, 256.0f);
        }
    }

    if (g->ntex == 0)
        return;                                 /* sin coordenadas: tc no sirve (la textura da negro) */

    /* La traslación en T1.xy. Dolphin la lleva en int con la vuelta de 32
     * bits y al final se queda con 24 (sext24): acá todo va módulo 2^24 */
    if (mtx != 0) {
        const uint8_t m = (uint8_t)(mtx - 1);
        switch (mid) {
        case 0:                                 /* S0.10 × s17.7, / 8 (menos de 2^20) */
            insp(g, RSXFP_DP3, T1, MX, R(T0), K(XYZW), NONE, GX_TEVFP_P_INDMTX_S, m);
            insp(g, RSXFP_DP3, T1, MY, R(T0), K(XYZW), NONE, GX_TEVFP_P_INDMTX_T, m);
            insk(g, RSXFP_MUL, T1, MXY, R(T1), K(SP(0)), NONE, 0.125f, 0, 0, 0);
            ins(g, RSXFP_FLR, T1, MXY, R(T1), NONE, NONE);
            break;
        case 1:
        case 2: {                               /* (s × ic) / 256 hacia 0, con s = 256 sh + sl */
            const rsxfp_src_t ic = RS(T0, SP(mid - 1));
            insk(g, RSXFP_MUL, T2, MXY, R(uv), K(SP(0)), NONE, 1.0f / 256.0f, -256, 0, 0);
            ins(g, RSXFP_FLR, T2, MXY, R(T2), NONE, NONE);                          /* sh */
            insk(g, RSXFP_MAD, T3, MXY, R(T2), K(SP(1)), R(uv), 1.0f / 256.0f, -256, 0, 0);  /* sl */
            ins(g, RSXFP_MUL, T2, MXY, R(T2), ic, NONE);                            /* a = sh ic */
            ins(g, RSXFP_MUL, T3, MXY, R(T3), ic, NONE);                            /* b = sl ic */
            insk(g, RSXFP_MUL, T1, MXY, R(T3), K(SP(0)), NONE, 1.0f / 256.0f, -256, 0, 0);
            ins(g, RSXFP_FLR, T1, MXY, R(T1), NONE, NONE);                          /* f = b >> 8 */
            insk(g, RSXFP_MAD, T3, MXY, R(T1), K(SP(1)), R(T3), 1.0f / 256.0f, -256, 0, 0);  /* r */
            ins(g, RSXFP_ADD, T2, MXY, R(T2), R(T1), NONE);                         /* q = a + f */
            insk(g, RSXFP_SLT, T1, MXY, R(T2), K(SP(0)), NONE, 0, 0, 0, 0);
            insk(g, RSXFP_SNE, T3, MXY, R(T3), K(SP(0)), NONE, 0, 0, 0, 0);
            ins(g, RSXFP_MUL, T1, MXY, R(T1), R(T3), NONE);                         /* q < 0 con resto */
            ins(g, RSXFP_ADD, T1, MXY, R(T2), R(T1), NONE);
            break;
        }
        default:                                /* inválida: tc queda como estaba */
            if (!g->tc_defined)
                g->p->stale |= GX_TEVFP_STALE_TC;
            return;
        }
        const uint32_t *r = &g->bp[BP_INDMTX + 3 * m];
        const int scale = (int)(bits(r[0], 22, 2) | bits(r[1], 22, 2) << 2 | bits(r[2], 22, 1) << 4);
        const int shift = 17 - scale;
        if (shift > 0) {
            insk(g, RSXFP_MUL, T1, MXY, R(T1), K(SP(0)), NONE, ldexpf(1.0f, -shift), 0, 0, 0);
            ins(g, RSXFP_FLR, T1, MXY, R(T1), NONE, NONE);
        } else if (shift < 0) {                 /* << k: solo cuentan los 24 - k bits de abajo */
            mod_n(g, T1, MXY, ldexpf(1.0f, 24 + shift));
            insk(g, RSXFP_MUL, T1, MXY, R(T1), K(SP(0)), NONE, ldexpf(1.0f, -shift), 0, 0, 0);
            sext24(g, T1, MXY);
        }
    }

    /* La envoltura de cada coordenada en T2.xy */
    const uint32_t wrap[2] = { sw, tw };
    for (uint32_t k = 0; k < 2; k++) {
        const uint8_t mk = (uint8_t)(1u << k);
        if (wrap[k] == 0) {
            ins(g, RSXFP_MOV, T2, mk, R(uv), NONE, NONE);
        } else if (wrap[k] <= 5) {
            ins(g, RSXFP_MOV, T2, mk, R(uv), NONE, NONE);
            mod_n(g, T2, mk, ldexpf(1.0f, 16 - (int)wrap[k]));
        } else {
            insk(g, RSXFP_MOV, T2, mk, K(XYZW), NONE, NONE, 0, 0, 0, 0);
        }
    }
    if (mtx != 0) {
        ins(g, RSXFP_ADD, T2, MXY, R(T2), R(T1), NONE);
        sext24(g, T2, MXY);
    }

    if (addprev) {
        if (!g->tc_defined)
            g->p->stale |= GX_TEVFP_STALE_TC;
        ins(g, RSXFP_ADD, R_TC, MXY, R(R_TC), R(T2), NONE);
        sext24(g, R_TC, MXY);
    } else {
        ins(g, RSXFP_MOV, R_TC, MXY, R(T2), NONE, NONE);
    }
    g->tc_defined = 1;
}

/* El swizzle de la tabla id (BP 0xF6 + 2 id y la siguiente) */
static uint8_t swap_swz(const uint32_t *bp, uint32_t id)
{
    const uint32_t rg = bp[BP_KSEL + 2 * id], ba = bp[BP_KSEL + 2 * id + 1];
    return SW(bits(rg, 0, 2), bits(rg, 2, 2), bits(ba, 0, 2), bits(ba, 2, 2));
}

/* Una entrada del combinador: rgb de carg y a de aarg en dst. Devuelve qué
 * componentes pueden quedar fuera de 0-255 (xyz = 1, w = 2) */
static int load_input(gen_t *g, uint8_t dst, uint32_t carg, uint32_t aarg, int konst_regs_c, int konst_regs_a)
{
    int creg = -1, cswz = XYZW, aregs = -1, wide = 0;
    float cval = 0, aval = 0;
    if (carg < 8) {
        creg = R_REG + (int)(carg >> 1);
        cswz = (carg & 1u) ? SP(3) : XYZW;
        wide |= 1;
    } else if (carg == 8 || carg == 9) {
        creg = R_TEX;
        cswz = carg == 9 ? SP(3) : XYZW;
    } else if (carg == 10 || carg == 11) {
        creg = R_RAS;
        cswz = carg == 11 ? SP(3) : XYZW;
    } else if (carg == 14) {
        creg = R_KON;
        if (konst_regs_c)
            wide |= 1;
    } else {
        cval = carg == 12 ? 255.0f : carg == 13 ? 128.0f : 0.0f;
    }
    if (aarg < 4) {
        aregs = R_REG + (int)aarg;
        wide |= 2;
    } else if (aarg == 4) {
        aregs = R_TEX;
    } else if (aarg == 5) {
        aregs = R_RAS;
    } else if (aarg == 6) {
        aregs = R_KON;
        if (konst_regs_a)
            wide |= 2;
    }
    if (((carg == 8 || carg == 9) || aarg == 4) && !g->tex_defined)
        g->p->stale |= GX_TEVFP_STALE_TEX;

    if (creg >= 0 && cswz == XYZW && aregs == creg) {
        ins(g, RSXFP_MOV, (int8_t)dst, MALL, R((uint8_t)creg), NONE, NONE);
        return wide;
    }
    if (creg < 0 && aregs < 0) {
        insk(g, RSXFP_MOV, (int8_t)dst, MALL, K(XYZW), NONE, NONE, cval, cval, cval, aval);
        return wide;
    }
    if (creg >= 0)
        ins(g, RSXFP_MOV, (int8_t)dst, MXYZ, RS((uint8_t)creg, (uint8_t)cswz), NONE, NONE);
    else
        insk(g, RSXFP_MOV, (int8_t)dst, MXYZ, K(XYZW), NONE, NONE, cval, cval, cval, 0);
    if (aregs >= 0)
        ins(g, RSXFP_MOV, (int8_t)dst, MW, RS((uint8_t)aregs, SP(3)), NONE, NONE);
    else
        insk(g, RSXFP_MOV, (int8_t)dst, MW, K(XYZW), NONE, NONE, 0, 0, 0, aval);
    return wide;
}

static const float bias_lut[4] = { 0, 128, -128, 0 };
static const float lshift_mul[4] = { 1, 2, 4, 1 };

static float rnd(uint32_t scale, uint32_t op)
{
    return scale == 3 ? 0.0f : op ? 127.0f : 128.0f;
}

/* Comparación: la etapa compara a con b y suma c a d si pasa */
static void compare(gen_t *g, uint32_t mode, int gt, int alpha)
{
    const uint8_t sop = gt ? RSXFP_SGT : RSXFP_SEQ;
    if (mode < 3) {
        static const float w[3][3] = { { 1, 0, 0 }, { 1, 256, 0 }, { 1, 256, 65536 } };
        insk(g, RSXFP_DP3, T0, MX, R(R_A), K(XYZW), NONE, w[mode][0], w[mode][1], w[mode][2], 0);
        insk(g, RSXFP_DP3, T0, MY, R(R_B), K(XYZW), NONE, w[mode][0], w[mode][1], w[mode][2], 0);
        ins(g, sop, T1, MX, RS(T0, SP(0)), RS(T0, SP(1)), NONE);
        if (alpha)
            ins(g, RSXFP_MAD, R_RES, MW, RS(T1, SP(0)), R(R_C), R(R_D));
        else
            ins(g, RSXFP_MAD, R_RES, MXYZ, RS(T1, SP(0)), R(R_C), R(R_D));
    } else if (alpha) {
        ins(g, sop, T1, MW, R(R_A), R(R_B), NONE);
        ins(g, RSXFP_MAD, R_RES, MW, R(T1), R(R_C), R(R_D));
    } else {
        ins(g, sop, T1, MXYZ, R(R_A), R(R_B), NONE);
        ins(g, RSXFP_MAD, R_RES, MXYZ, R(T1), R(R_C), R(R_D));
    }
}

static void stage(gen_t *g, uint32_t s)
{
    const uint32_t *bp = g->bp;
    const uint32_t order = bp[BP_TEVORDER + (s >> 1)];
    const unsigned sh = (s & 1u) ? 12 : 0;
    const uint32_t cc = bp[BP_COMBINER + 2 * s], ac = bp[BP_COMBINER + 2 * s + 1];
    uint32_t sel = bits(order, sh + 3, 3);
    const uint32_t texmap = bits(order, sh, 3);
    if (sel >= g->ntex)
        sel = 0;

    stage_indirect(g, s, sel);

    if (bits(order, sh + 6, 1)) {
        if (g->ntex > 0)
            sample(g, R_RAW, R_TC, (uint8_t)texmap, 0, (uint8_t)s);
        else
            insk(g, RSXFP_MOV, R_RAW, MALL, K(XYZW), NONE, NONE, 0, 0, 0, 0);
        ins(g, RSXFP_MOV, R_TEX, MALL, RS(R_RAW, swap_swz(bp, bits(ac, 2, 2))), NONE, NONE);
        g->tex_defined = 1;
    }

    const uint32_t ca = bits(cc, 12, 4), cb = bits(cc, 8, 4), cx = bits(cc, 4, 4), cd = bits(cc, 0, 4);
    const uint32_t aa = bits(ac, 13, 3), ab = bits(ac, 10, 3), ax = bits(ac, 7, 3), ad = bits(ac, 4, 3);

    /* La constante de la etapa, si se usa */
    const uint32_t ksel = bp[BP_KSEL + (s >> 1)];
    const uint32_t kc = (s & 1u) ? bits(ksel, 14, 5) : bits(ksel, 4, 5);
    const uint32_t ka = (s & 1u) ? bits(ksel, 19, 5) : bits(ksel, 9, 5);
    if (ca == 14 || cb == 14 || cx == 14 || cd == 14 || aa == 6 || ab == 6 || ax == 6 || ad == 6)
        insp(g, RSXFP_MOV, R_KON, MALL, K(XYZW), NONE, NONE, GX_TEVFP_P_KONST, (uint8_t)s);

    /* El color rasterizado, si se usa */
    if (ca == 10 || ca == 11 || cb == 10 || cb == 11 || cx == 10 || cx == 11 || cd == 10 || cd == 11 ||
        aa == 5 || ab == 5 || ax == 5 || ad == 5) {
        const uint32_t chan = bits(order, sh + 7, 3);
        if (chan == 0 || chan == 1) {
            if (chan >= g->ncol)
                g->p->stale |= GX_TEVFP_STALE_COLOR;
            ins(g, RSXFP_MOV, R_RAS, MALL, RS((uint8_t)(R_COL + chan), swap_swz(bp, bits(ac, 0, 2))), NONE,
                NONE);
        } else if (chan == 5) {
            ins(g, RSXFP_MOV, R_RAS, MALL, RS(R_BUMP, SP(0)), NONE, NONE);
        } else if (chan == 6) {
            insk(g, RSXFP_MUL, T0, MX, RS(R_BUMP, SP(0)), K(SP(0)), NONE, 1.0f / 32.0f, 0, 0, 0);
            ins(g, RSXFP_FLR, T0, MX, R(T0), NONE, NONE);
            ins(g, RSXFP_ADD, R_RAS, MALL, RS(R_BUMP, SP(0)), RS(T0, SP(0)), NONE);
        } else {
            insk(g, RSXFP_MOV, R_RAS, MALL, K(XYZW), NONE, NONE, 0, 0, 0, 0);
        }
    }

    /* Entradas: a, b y c de 8 bits sin signo, d de 11 con signo (ya lo es) */
    const int kwide_c = kc >= 12, kwide_a = ka >= 12;
    const uint8_t dsts[4] = { R_A, R_B, R_C, R_D };
    const uint32_t cargs[4] = { ca, cb, cx, cd }, aargs[4] = { aa, ab, ax, ad };
    for (int k = 0; k < 4; k++) {
        const int wide = load_input(g, dsts[k], cargs[k], aargs[k], kwide_c, kwide_a);
        if (k < 3 && wide)
            mod_n(g, dsts[k], (uint8_t)((wide & 1 ? MXYZ : 0) | (wide & 2 ? MW : 0)), 256.0f);
    }

    const int creg = bits(cc, 16, 2) != 3, areg = bits(ac, 16, 2) != 3;
    const uint32_t cs = bits(cc, 20, 2), cop = bits(cc, 18, 1), cbias = bits(cc, 16, 2);
    const uint32_t as = bits(ac, 20, 2), aop = bits(ac, 18, 1), abias = bits(ac, 16, 2);

    if (creg || areg) {
        /* c' = c + (c >> 7); temp = a × (256 - c') + b × c' = 256 a + c' (b - a) */
        insk(g, RSXFP_MUL, T0, MALL, R(R_C), K(SP(0)), NONE, 1.0f / 128.0f, 0, 0, 0);
        ins(g, RSXFP_FLR, T0, MALL, R(T0), NONE, NONE);
        ins(g, RSXFP_ADD, T0, MALL, R(T0), R(R_C), NONE);
        ins(g, RSXFP_ADD, T1, MALL, R(R_B), NEG(R(R_A)), NONE);
        ins(g, RSXFP_MUL, T1, MALL, R(T1), R(T0), NONE);
        insk(g, RSXFP_MAD, T1, MALL, R(R_A), K(SP(0)), R(T1), 256, 0, 0, 0);
        /* << lshift, + redondeo */
        const float lc = lshift_mul[cs], la = lshift_mul[as];
        if (lc != 1 || la != 1)
            insk(g, RSXFP_MUL, T1, MALL, R(T1), K(XYZW), NONE, lc, lc, lc, la);
        const float rc = rnd(cs, cop), ra = rnd(as, aop);
        if (rc != 0 || ra != 0)
            insk(g, RSXFP_ADD, T1, MALL, R(T1), K(XYZW), NONE, rc, rc, rc, ra);
        /* >> 8: el color resta después de correr, el alfa antes */
        const float s1a = aop ? -1.0f / 256.0f : 1.0f / 256.0f;
        insk(g, RSXFP_MUL, T1, MALL, R(T1), K(XYZW), NONE, 1.0f / 256.0f, 1.0f / 256.0f, 1.0f / 256.0f, s1a);
        ins(g, RSXFP_FLR, T1, MALL, R(T1), NONE, NONE);
        if (cop)
            insk(g, RSXFP_MUL, T1, MXYZ, R(T1), K(SP(0)), NONE, -1, 0, 0, 0);
        /* (d + sesgo) << lshift + temp, >> rshift */
        const float bc = bias_lut[cbias == 3 ? 0 : cbias], ba = bias_lut[abias == 3 ? 0 : abias];
        uint8_t dsrc = R_D;
        if (bc != 0 || ba != 0) {
            insk(g, RSXFP_ADD, T2, MALL, R(R_D), K(XYZW), NONE, bc, bc, bc, ba);
            dsrc = T2;
        }
        if (lc != 1 || la != 1)
            insk(g, RSXFP_MAD, R_RES, MALL, R(dsrc), K(XYZW), R(T1), lc, lc, lc, la);
        else
            ins(g, RSXFP_ADD, R_RES, MALL, R(dsrc), R(T1), NONE);
        const float hc = cs == 3 ? 0.5f : 1.0f, ha = as == 3 ? 0.5f : 1.0f;
        if (hc != 1 || ha != 1) {
            insk(g, RSXFP_MUL, R_RES, MALL, R(R_RES), K(XYZW), NONE, hc, hc, hc, ha);
            ins(g, RSXFP_FLR, R_RES, MALL, R(R_RES), NONE, NONE);
        }
    }
    if (!creg)
        compare(g, bits(cc, 20, 2), !bits(cc, 18, 1), 0);
    if (!areg)
        compare(g, bits(ac, 20, 2), !bits(ac, 18, 1), 1);

    /* Recorte a 0-255 o a -1024..1023 */
    const float loc = bits(cc, 19, 1) ? 0.0f : -1024.0f, hic = bits(cc, 19, 1) ? 255.0f : 1023.0f;
    const float loa = bits(ac, 19, 1) ? 0.0f : -1024.0f, hia = bits(ac, 19, 1) ? 255.0f : 1023.0f;
    insk(g, RSXFP_MAX, R_RES, MALL, R(R_RES), K(XYZW), NONE, loc, loc, loc, loa);
    insk(g, RSXFP_MIN, R_RES, MALL, R(R_RES), K(XYZW), NONE, hic, hic, hic, hia);

    const uint32_t cdest = bits(cc, 22, 2), adest = bits(ac, 22, 2);
    if (cdest == adest) {
        ins(g, RSXFP_MOV, (int8_t)(R_REG + cdest), MALL, R(R_RES), NONE, NONE);
    } else {
        ins(g, RSXFP_MOV, (int8_t)(R_REG + cdest), MXYZ, R(R_RES), NONE, NONE);
        ins(g, RSXFP_MOV, (int8_t)(R_REG + adest), MW, R(R_RES), NONE, NONE);
    }
}

/* La prueba de alfa sobre R_OUTI.w: KIL si no pasa */
static void alpha_test(gen_t *g)
{
    const uint32_t at = g->bp[BP_ALPHATEST];
    const uint32_t comp[2] = { bits(at, 16, 3), bits(at, 19, 3) }, logic = bits(at, 22, 2);
    static const uint8_t cmp_op[8] = { 0, RSXFP_SLT, RSXFP_SEQ, RSXFP_SLE, RSXFP_SGT, RSXFP_SNE, RSXFP_SGE, 0 };
    int known[2];                               /* -1: se calcula */
    for (int k = 0; k < 2; k++)
        known[k] = comp[k] == 0 ? 0 : comp[k] == 7 ? 1 : -1;
    if (known[0] >= 0 && known[1] >= 0) {
        int pass;
        switch (logic) {
        case 0: pass = known[0] && known[1]; break;
        case 1: pass = known[0] || known[1]; break;
        case 2: pass = known[0] ^ known[1]; break;
        default: pass = !(known[0] ^ known[1]); break;
        }
        if (!pass) {
            rsxfp_insn_t i;
            memset(&i, 0, sizeof i);
            i.op = RSXFP_KIL;
            i.dst = RSXFP_NO_DST;
            i.mask = MALL;
            i.cond = RSXFP_TR;
            i.cond_swz = XYZW;
            const int32_t r = rsxfp_emit(&g->fp, &i);
            if (r < -1 && !g->err)
                g->err = r;
        }
        return;
    }
    for (int k = 0; k < 2; k++) {
        const uint8_t mk = (uint8_t)(1u << k);
        if (known[k] >= 0)
            insk(g, RSXFP_MOV, T0, mk, K(SP(0)), NONE, NONE, (float)known[k], 0, 0, 0);
        else
            insp(g, cmp_op[comp[k]], T0, mk, RS(R_OUTI, SP(3)), K(SP((uint8_t)k)), NONE, GX_TEVFP_P_ALPHAREF, 0);
    }
    switch (logic) {
    case 0:
        ins(g, RSXFP_MUL, T0, MZ, RS(T0, SP(0)), RS(T0, SP(1)), NONE);
        ins_full(g, RSXFP_ADD, RSXFP_NO_DST, MX, RS(T0, SP(2)), K(SP(0)), NONE, (const float[4]){ -0.5f, 0, 0, 0 },
                 0, 1);
        break;
    case 1:
        ins(g, RSXFP_MAX, T0, MZ, RS(T0, SP(0)), RS(T0, SP(1)), NONE);
        ins_full(g, RSXFP_ADD, RSXFP_NO_DST, MX, RS(T0, SP(2)), K(SP(0)), NONE, (const float[4]){ -0.5f, 0, 0, 0 },
                 0, 1);
        break;
    case 2:
        ins(g, RSXFP_ADD, T0, MZ, RS(T0, SP(0)), NEG(RS(T0, SP(1))), NONE);
        ins_full(g, RSXFP_ADD, RSXFP_NO_DST, MX, ABS(RS(T0, SP(2))), K(SP(0)), NONE,
                 (const float[4]){ -0.5f, 0, 0, 0 }, 0, 1);
        break;
    default:
        ins(g, RSXFP_ADD, T0, MZ, RS(T0, SP(0)), NEG(RS(T0, SP(1))), NONE);
        ins_full(g, RSXFP_ADD, RSXFP_NO_DST, MX, NEG(ABS(RS(T0, SP(2)))), K(SP(0)), NONE,
                 (const float[4]){ 0.5f, 0, 0, 0 }, 0, 1);
        break;
    }
    rsxfp_insn_t i;
    memset(&i, 0, sizeof i);
    i.op = RSXFP_KIL;
    i.dst = RSXFP_NO_DST;
    i.mask = MALL;
    i.cond = RSXFP_LT;
    i.cond_swz = SP(0);
    const int32_t r = rsxfp_emit(&g->fp, &i);
    if (r < -1 && !g->err)
        g->err = r;
}

/* La textura de Z: R_Z.x = (sesgo + texel (+ z)) & 0xFFFFFF, en 12 + 12 bits */
static void ztex(gen_t *g, uint32_t op, uint32_t fmt)
{
    if (!g->tex_defined)
        g->p->stale |= GX_TEVFP_STALE_TEX;
    switch (fmt) {
    case 0:                                     /* U8: a */
        insk(g, RSXFP_MOV, T1, MX, K(SP(0)), NONE, NONE, 0, 0, 0, 0);
        ins(g, RSXFP_MOV, T1, MY, RS(R_RAW, SP(3)), NONE, NONE);
        break;
    case 1:                                     /* U16: a << 8 | r */
        insk(g, RSXFP_MUL, T1, MX, RS(R_RAW, SP(3)), K(SP(0)), NONE, 1.0f / 16.0f, 0, 0, 0);
        ins(g, RSXFP_FLR, T1, MX, R(T1), NONE, NONE);
        insk(g, RSXFP_MAD, T2, MX, RS(T1, SP(0)), K(SP(0)), RS(R_RAW, SP(3)), -16, 256, 0, 0);
        insk(g, RSXFP_MAD, T1, MY, RS(T2, SP(0)), K(SP(1)), RS(R_RAW, SP(0)), -16, 256, 0, 0);
        break;
    case 2:                                     /* U24: r << 16 | g << 8 | b */
        insk(g, RSXFP_MUL, T2, MX, RS(R_RAW, SP(1)), K(SP(0)), NONE, 1.0f / 16.0f, 16, 0, 0);
        ins(g, RSXFP_FLR, T2, MX, R(T2), NONE, NONE);
        insk(g, RSXFP_MAD, T1, MX, RS(R_RAW, SP(0)), K(SP(1)), RS(T2, SP(0)), 1.0f / 16.0f, 16, 0, 0);
        insk(g, RSXFP_MAD, T2, MY, RS(T2, SP(0)), K(SP(0)), RS(R_RAW, SP(1)), -16, 256, 0, 0);
        insk(g, RSXFP_MAD, T1, MY, RS(T2, SP(1)), K(SP(1)), RS(R_RAW, SP(2)), -16, 256, 0, 0);
        break;
    default:                                    /* inválido: solo el sesgo */
        insk(g, RSXFP_MOV, T1, MXY, K(XYZW), NONE, NONE, 0, 0, 0, 0);
        break;
    }
    insp(g, RSXFP_ADD, T1, MXY, R(T1), K(XYZW), NONE, GX_TEVFP_P_ZTEX, 0);
    if (op == 1) {                              /* sumar la Z */
        insk(g, RSXFP_MUL, T2, MX, RS(R_Z, SP(0)), K(SP(0)), NONE, 1.0f / 4096.0f, -4096, 0, 0);
        ins(g, RSXFP_FLR, T2, MX, R(T2), NONE, NONE);
        insk(g, RSXFP_MAD, T2, MY, RS(T2, SP(0)), K(SP(1)), RS(R_Z, SP(0)), 1.0f / 4096.0f, -4096, 0, 0);
        ins(g, RSXFP_ADD, T1, MXY, R(T1), R(T2), NONE);
    }
    /* Acarreo de la parte baja y la alta módulo 4096 */
    insk(g, RSXFP_MUL, T2, MX, RS(T1, SP(1)), K(SP(0)), NONE, 1.0f / 4096.0f, -4096, 0, 0);
    ins(g, RSXFP_FLR, T2, MX, R(T2), NONE, NONE);
    insk(g, RSXFP_MAD, T1, MY, RS(T2, SP(0)), K(SP(1)), RS(T1, SP(1)), 1.0f / 4096.0f, -4096, 0, 0);
    ins(g, RSXFP_ADD, T1, MX, RS(T1, SP(0)), RS(T2, SP(0)), NONE);
    mod_n(g, T1, MX, 4096.0f);
    insk(g, RSXFP_MAD, R_Z, MX, RS(T1, SP(0)), K(SP(0)), RS(T1, SP(1)), 4096, 0, 0, 0);
    insk(g, RSXFP_MUL, R_DEPTH, MZ, RS(R_Z, SP(0)), K(SP(0)), NONE, 1.0f / 16777215.0f, 0, 0, 0);
    g->fp.depth = 1;
    g->p->ztex = 1;
}

/* k del rango para el índice de idx (un componente de T1) en T2.<dst> */
static void fog_k_lookup(gen_t *g, uint8_t idx_comp, uint8_t dst_mask)
{
    for (uint8_t j = 0; j < 3; j++) {
        const float b = (float)(4 * j);
        insk(g, RSXFP_SEQ, T3, MALL, RS(T1, SP(idx_comp)), K(XYZW), NONE, b, b + 1, b + 2, b + 3);
        if (j == 0) {
            insp(g, RSXFP_DP4, T2, dst_mask, R(T3), K(XYZW), NONE, GX_TEVFP_P_FOGK, j);
        } else {
            insp(g, RSXFP_DP4, T3, MX, R(T3), K(XYZW), NONE, GX_TEVFP_P_FOGK, j);
            ins(g, RSXFP_ADD, T2, dst_mask, R(T2), RS(T3, SP(0)), NONE);
        }
    }
}

/* La niebla sobre R_OUTI.xyz con la Z de R_Z.x */
static void fog(gen_t *g)
{
    const uint32_t c = g->bp[BP_FOG_C];
    const uint32_t fsel = bits(c, 21, 3);
    g->p->fog = 1;
    if (!bits(c, 20, 1)) {                      /* perspectiva: A × (2^24 - 1) / (B - (Z >> corrimiento)) */
        insp(g, RSXFP_MUL, T1, MX, RS(R_Z, SP(0)), K(SP(3)), NONE, GX_TEVFP_P_FOG, 0);
        ins(g, RSXFP_FLR, T1, MX, R(T1), NONE, NONE);
        insp(g, RSXFP_ADD, T1, MX, NEG(RS(T1, SP(0))), K(SP(2)), NONE, GX_TEVFP_P_FOG, 0);
        ins(g, RSXFP_RCP, T1, MX, RS(T1, SP(0)), NONE, NONE);
        insp(g, RSXFP_MUL, T0, MX, RS(T1, SP(0)), K(SP(0)), NONE, GX_TEVFP_P_FOG, 0);
    } else {                                    /* ortográfica: A × Z / (2^24 - 1) */
        insk(g, RSXFP_MUL, T1, MX, RS(R_Z, SP(0)), K(SP(0)), NONE, 1.0f / 16777215.0f, 0, 0, 0);
        insp(g, RSXFP_MUL, T0, MX, RS(T1, SP(0)), K(SP(0)), NONE, GX_TEVFP_P_FOG, 0);
    }

    if (bits(g->bp[BP_FOGRANGE], 10, 1)) {
        ins(g, RSXFP_FLR, T1, MX, IN(RSXFP_WPOS, SP(0)), NONE, NONE);
        insp(g, RSXFP_ADD, T1, MX, RS(T1, SP(0)), NEG(K(SP(0))), NONE, GX_TEVFP_P_FOGRANGE, 0);
        insp(g, RSXFP_MUL, T1, MX, RS(T1, SP(0)), K(SP(1)), NONE, GX_TEVFP_P_FOGRANGE, 0);  /* offset */
        insk(g, RSXFP_MAD, T1, MY, ABS(RS(T1, SP(0))), K(SP(0)), K(SP(1)), -9, 9, 0, 0);
        insk(g, RSXFP_MAX, T1, MY, RS(T1, SP(1)), K(SP(0)), NONE, 0, 9, 0, 0);
        insk(g, RSXFP_MIN, T1, MY, RS(T1, SP(1)), K(SP(1)), NONE, 0, 9, 0, 0);
        ins(g, RSXFP_FLR, T1, MZ, RS(T1, SP(1)), NONE, NONE);                          /* lower */
        insk(g, RSXFP_ADD, T1, MW, RS(T1, SP(2)), K(SP(0)), NONE, 1, 0, 0, 0);        /* upper */
        fog_k_lookup(g, 2, MX);                                                      /* kl */
        fog_k_lookup(g, 3, MY);                                                      /* ku */
        ins(g, RSXFP_ADD, T1, MZ, RS(T1, SP(3)), NEG(RS(T1, SP(1))), NONE);           /* factor */
        insk(g, RSXFP_ADD, T3, MX, NEG(RS(T1, SP(2))), K(SP(0)), NONE, 1, 0, 0, 0);    /* 1 - factor */
        ins(g, RSXFP_MUL, T3, MY, RS(T2, SP(1)), RS(T3, SP(0)), NONE);
        ins(g, RSXFP_MAD, T3, MZ, RS(T2, SP(0)), RS(T1, SP(2)), RS(T3, SP(1)));       /* k */
        ins(g, RSXFP_MUL, T3, MX, RS(T1, SP(0)), RS(T1, SP(0)), NONE);
        ins(g, RSXFP_MAD, T3, MX, RS(T3, SP(2)), RS(T3, SP(2)), RS(T3, SP(0)));
        ins(g, RSXFP_LG2, T3, MY, RS(T3, SP(0)), NONE, NONE);                          /* sqrt */
        insk(g, RSXFP_MUL, T3, MY, RS(T3, SP(1)), K(SP(0)), NONE, 0.5f, 0, 0, 0);
        ins(g, RSXFP_EX2, T3, MY, RS(T3, SP(1)), NONE, NONE);
        ins(g, RSXFP_RCP, T3, MW, RS(T3, SP(2)), NONE, NONE);
        ins(g, RSXFP_MUL, T3, MY, RS(T3, SP(1)), RS(T3, SP(3)), NONE);
        ins(g, RSXFP_MUL, T0, MX, RS(T0, SP(0)), RS(T3, SP(1)), NONE);
    }

    insp(g, RSXFP_ADD, T0, MX, RS(T0, SP(0)), NEG(K(SP(1))), NONE, GX_TEVFP_P_FOG, 0);
    ins_full(g, RSXFP_MOV, T0, MX, RS(T0, SP(0)), NONE, NONE, NULL, 1, 0);
    switch (fsel) {
    case 4:                                     /* Exp: 1 - 2^(-8 f) */
        insk(g, RSXFP_MUL, T1, MX, RS(T0, SP(0)), K(SP(0)), NONE, -8, 1, 0, 0);
        ins(g, RSXFP_EX2, T1, MX, RS(T1, SP(0)), NONE, NONE);
        insk(g, RSXFP_ADD, T0, MX, NEG(RS(T1, SP(0))), K(SP(1)), NONE, -8, 1, 0, 0);
        break;
    case 5:                                     /* ExpSq: 1 - 2^(-8 f²) */
        ins(g, RSXFP_MUL, T1, MX, RS(T0, SP(0)), RS(T0, SP(0)), NONE);
        insk(g, RSXFP_MUL, T1, MX, RS(T1, SP(0)), K(SP(0)), NONE, -8, 1, 0, 0);
        ins(g, RSXFP_EX2, T1, MX, RS(T1, SP(0)), NONE, NONE);
        insk(g, RSXFP_ADD, T0, MX, NEG(RS(T1, SP(0))), K(SP(1)), NONE, -8, 1, 0, 0);
        break;
    case 6:                                     /* BackwardsExp: 2^(-8 (1 - f)) */
        insk(g, RSXFP_ADD, T1, MX, NEG(RS(T0, SP(0))), K(SP(1)), NONE, -8, 1, 0, 0);
        insk(g, RSXFP_MUL, T1, MX, RS(T1, SP(0)), K(SP(0)), NONE, -8, 1, 0, 0);
        ins(g, RSXFP_EX2, T0, MX, RS(T1, SP(0)), NONE, NONE);
        break;
    case 7:                                     /* BackwardsExpSq: 2^(-8 (1 - f)²) */
        insk(g, RSXFP_ADD, T1, MX, NEG(RS(T0, SP(0))), K(SP(1)), NONE, -8, 1, 0, 0);
        ins(g, RSXFP_MUL, T1, MX, RS(T1, SP(0)), RS(T1, SP(0)), NONE);
        insk(g, RSXFP_MUL, T1, MX, RS(T1, SP(0)), K(SP(0)), NONE, -8, 1, 0, 0);
        ins(g, RSXFP_EX2, T0, MX, RS(T1, SP(0)), NONE, NONE);
        break;
    default:                                    /* lineal */
        break;
    }
    /* fi = f × 256 hacia 0; out = (out × (256 - fi) + fi × color) >> 8 */
    insk(g, RSXFP_MUL, T0, MX, RS(T0, SP(0)), K(SP(0)), NONE, 256, 0, 0, 0);
    ins(g, RSXFP_FLR, T0, MX, RS(T0, SP(0)), NONE, NONE);
    insp(g, RSXFP_ADD, T1, MXYZ, NEG(R(R_OUTI)), K(XYZW), NONE, GX_TEVFP_P_FOGCOLOR, 0);
    ins(g, RSXFP_MUL, T1, MXYZ, R(T1), RS(T0, SP(0)), NONE);
    insk(g, RSXFP_MAD, T1, MXYZ, R(R_OUTI), K(SP(0)), R(T1), 256, 1.0f / 256.0f, 0, 0);
    insk(g, RSXFP_MUL, T1, MXYZ, R(T1), K(SP(1)), NONE, 256, 1.0f / 256.0f, 0, 0);
    ins(g, RSXFP_FLR, R_OUTI, MXYZ, R(T1), NONE, NONE);
}

int gx_tevfp_build(const uint32_t bp[256], gx_tevfp_t *p)
{
    gen_t g;
    memset(&g, 0, sizeof g);
    memset(&p->info, 0, sizeof p->info);
    p->npatch = p->ntex = p->stale = 0;
    p->fog = p->ztex = 0;
    rsxfp_init(&g.fp, p->words, GX_TEVFP_WORDS);
    g.p = p;
    g.bp = bp;
    const uint32_t gm = bp[BP_GENMODE];
    g.ntex = bits(gm, 0, 4);
    g.ncol = bits(gm, 4, 3) < 2 ? bits(gm, 4, 3) : 2;
    g.nstages = bits(gm, 10, 4);
    g.nind = bits(gm, 16, 3) < 4 ? bits(gm, 16, 3) : 4;

    /* Las coordenadas que se leen: la de cada etapa y la de cada indirecta */
    uint32_t need_uv = 0;
    if (g.ntex > 0) {
        for (uint32_t s = 0; s <= g.nstages; s++) {
            uint32_t sel = bits(bp[BP_TEVORDER + (s >> 1)], (s & 1u) ? 15 : 3, 3);
            need_uv |= 1u << (sel >= g.ntex ? 0 : sel);
        }
        for (uint32_t s = 0; s < g.nind; s++) {
            uint32_t sel = bits(bp[BP_IREF], 6 * s + 3, 3);
            need_uv |= 1u << (sel >= g.ntex ? 0 : sel);
        }
        if (g.ntex > 8 && (need_uv & ~0xFFu))
            p->stale |= GX_TEVFP_STALE_TC;
    }

    load_colors(&g);
    for (uint32_t i = 0; i < 8; i++)
        if (need_uv & (1u << i))
            load_uv(&g, i);
    indirect_lookups(&g);
    for (uint32_t i = 0; i < 4; i++)
        insp(&g, RSXFP_MOV, (int8_t)(R_REG + i), MALL, K(XYZW), NONE, NONE, GX_TEVFP_P_REG, (uint8_t)i);
    for (uint32_t s = 0; s <= g.nstages; s++)
        stage(&g, s);

    /* Lo de la última etapa, (uint8_t) */
    const uint32_t cdl = bits(bp[BP_COMBINER + 2 * g.nstages], 22, 2);
    const uint32_t adl = bits(bp[BP_COMBINER + 2 * g.nstages + 1], 22, 2);
    ins(&g, RSXFP_MOV, R_OUTI, MXYZ, R((uint8_t)(R_REG + cdl)), NONE, NONE);
    ins(&g, RSXFP_MOV, R_OUTI, MW, R((uint8_t)(R_REG + adl)), NONE, NONE);
    mod_n(&g, R_OUTI, MALL, 256.0f);

    alpha_test(&g);

    const uint32_t ztex2 = bp[BP_ZTEX2];
    const uint32_t fsel = bits(bp[BP_FOG_C], 21, 3);
    if (bits(ztex2, 2, 2) != 0 || fsel != 0) {  /* la Z: f2i(clamp(z, 0, 2^24 - 1)) */
        ins(&g, RSXFP_MOV, T0, MX, IN(RSXFP_FOGC, SP(0)), NONE, NONE);
        insk(&g, RSXFP_MAX, T0, MX, RS(T0, SP(0)), K(SP(0)), NONE, 0, 16777215, 0, 0);
        insk(&g, RSXFP_MIN, T0, MX, RS(T0, SP(0)), K(SP(1)), NONE, 0, 16777215, 0, 0);
        ins(&g, RSXFP_FLR, R_Z, MX, RS(T0, SP(0)), NONE, NONE);
    }
    if (bits(ztex2, 2, 2) != 0)
        ztex(&g, bits(ztex2, 2, 2), bits(ztex2, 0, 2));
    if (fsel != 0)
        fog(&g);

    insk(&g, RSXFP_MUL, R_OUT, MALL, R(R_OUTI), K(SP(0)), NONE, 1.0f / 255.0f, 0, 0, 0);
    if (g.err)
        return g.err;
    return rsxfp_finish(&g.fp, &p->info);
}

/* ---- constantes de cada dibujo ---------------------------------------------------------- */

static void put(uint32_t *words, uint32_t at, float a, float b, float c, float d)
{
    const float v[4] = { a, b, c, d };
    memcpy(&words[at], v, sizeof v);
}

void gx_tevfp_patch(const gc_gx_t *gx, const gx_tevfp_t *p, uint32_t *words, int raw_coords)
{
    const uint32_t *bp = gx->bp;
    for (uint32_t n = 0; n < p->npatch; n++) {
        const uint32_t at = p->patch[n].at, arg = p->patch[n].arg;
        switch (p->patch[n].kind) {
        case GX_TEVFP_P_REG: {
            const int16_t *c = gx->tev_color[arg];
            put(words, at, c[0], c[1], c[2], c[3]);
            break;
        }
        case GX_TEVFP_P_KONST: {
            const uint32_t ksel = bp[BP_KSEL + (arg >> 1)];
            const uint32_t kc = (arg & 1u) ? bits(ksel, 14, 5) : bits(ksel, 4, 5);
            const uint32_t ka = (arg & 1u) ? bits(ksel, 19, 5) : bits(ksel, 9, 5);
            int16_t k[4][4], a, r, g, b, r2, g2, b2;    /* en el orden de gx_tev_t: a, b, g, r */
            for (int i = 0; i < 4; i++) {
                k[i][GX_RED] = gx->tev_kcolor[i][0];
                k[i][GX_GRN] = gx->tev_kcolor[i][1];
                k[i][GX_BLU] = gx->tev_kcolor[i][2];
                k[i][GX_ALP] = gx->tev_kcolor[i][3];
            }
            gx_tev_konst_lut((const int16_t (*)[4])k, kc, &a, &r, &g, &b);
            gx_tev_konst_lut((const int16_t (*)[4])k, ka, &a, &r2, &g2, &b2);
            put(words, at, r, g, b, a);
            break;
        }
        case GX_TEVFP_P_INDMTX_S:
        case GX_TEVFP_P_INDMTX_T: {
            const uint32_t *m = &bp[BP_INDMTX + 3 * arg];
            const unsigned f = p->patch[n].kind == GX_TEVFP_P_INDMTX_S ? 0 : 11;
            put(words, at, (float)sbits(m[0], f, 11), (float)sbits(m[1], f, 11), (float)sbits(m[2], f, 11), 0);
            break;
        }
        case GX_TEVFP_P_TEXSCALE: {
            if (raw_coords) {
                put(words, at, 1, 1, 0, 0);
                break;
            }
            const uint32_t img = bp[(arg < 4 ? BP_SETIMAGE0 : BP_SETIMAGE0_4) + (arg & 3u)];
            const float w = (float)(bits(img, 0, 10) + 1), h = (float)(bits(img, 10, 10) + 1);
            put(words, at, 1.0f / (128.0f * w), 1.0f / (128.0f * h), 0, 0);
            break;
        }
        case GX_TEVFP_P_ALPHAREF: {
            const uint32_t a = bp[BP_ALPHATEST];
            put(words, at, (float)bits(a, 0, 8), (float)bits(a, 8, 8), 0, 0);
            break;
        }
        case GX_TEVFP_P_ZTEX: {
            const uint32_t b = bits(bp[BP_ZTEX1], 0, 24);
            put(words, at, (float)(b >> 12), (float)(b & 4095u), 0, 0);
            break;
        }
        case GX_TEVFP_P_FOG: {
            const int persp = !bits(bp[BP_FOG_C], 20, 1);
            const float a = gx_tev_fog_a(gx);
            put(words, at, persp ? a * 16777215.0f : a, gx_tev_fog_c(gx), (float)bp[BP_FOG_BMAG],
                ldexpf(1.0f, -(int)(bp[BP_FOG_BSHIFT] & 31u)));
            break;
        }
        case GX_TEVFP_P_FOGRANGE: {
            float wd;
            memcpy(&wd, &gx->xf[XF_VIEWPORT_WD], sizeof wd);
            put(words, at, (float)((int32_t)bits(bp[BP_FOGRANGE], 0, 10) - 342), 1.0f / wd, 0, 0);
            break;
        }
        case GX_TEVFP_P_FOGK: {
            float k[4];
            for (int j = 0; j < 4; j++) {
                const int32_t idx = (int32_t)(4 * arg) + j;
                k[j] = idx <= 10 ? gx_tev_fog_k(gx, idx) * 4.0f : 0.0f;
            }
            put(words, at, k[0], k[1], k[2], k[3]);
            break;
        }
        case GX_TEVFP_P_FOGCOLOR: {
            const uint32_t c = bp[BP_FOG_COLOR];
            put(words, at, (float)bits(c, 16, 8), (float)bits(c, 8, 8), (float)bits(c, 0, 8), 0);
            break;
        }
        default:
            break;
        }
    }
}
