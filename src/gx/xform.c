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
 * TransformUnit.cpp de Dolphin en C (ver xform.h). Cada expresión conserva el
 * orden de Dolphin: a * b + c * d + e se evalúa ((a*b) + (c*d)) + e, y nunca
 * se fusiona (-ffp-contract=off en todas las compilaciones).
 *
 * Los colores de Dolphin están en orden abgr en un u8[4] (memoria de x86);
 * acá van por canal (r, g, b) con las mismas cuentas: cada canal se calcula
 * igual y por separado, así que da lo mismo.
 */
#include <math.h>
#include <string.h>

#include "gx/xform.h"

/* Registros de XF, desde 0x1000 */
enum {
    XF_NUMCHAN = 0x09, XF_AMB0 = 0x0A, XF_MAT0 = 0x0C, XF_COLOR0 = 0x0E, XF_ALPHA0 = 0x10,
    XF_DUALTEX = 0x12, XF_PROJ = 0x20, XF_PROJ_TYPE = 0x26, XF_NUMTEXGEN = 0x3F,
    XF_TEXMTXINFO = 0x40, XF_POSTMTXINFO = 0x50,
};

/* Memoria de XF */
enum { XF_NORMAL_MTX = 0x400, XF_POST_MTX = 0x500, XF_LIGHTS = 0x600 };

typedef struct vec3 {
    float x, y, z;
} vec3_t;

static inline float word_f(const gx_xf_snap_t *xf, uint32_t at)
{
    return xf->mem[at];
}

static inline uint32_t word_u(const gx_xf_snap_t *xf, uint32_t at)
{
    uint32_t u;
    memcpy(&u, &xf->mem[at], sizeof u);
    return u;
}

static inline const float *mat_at(const gx_xf_snap_t *xf, uint32_t at)
{
    return &xf->mem[at];
}

static inline float dot(vec3_t a, vec3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3_t vdiv(vec3_t a, float s)
{
    vec3_t r = { a.x / s, a.y / s, a.z / s };
    return r;
}

static inline vec3_t normalized(vec3_t a)
{
    return vdiv(a, sqrtf(dot(a, a)));
}

static inline vec3_t vsub(vec3_t a, vec3_t b)
{
    vec3_t r = { a.x - b.x, a.y - b.y, a.z - b.z };
    return r;
}

/* std::max(0.0f, v): con NaN da 0 */
static inline float max0(float v)
{
    return 0.0f < v ? v : 0.0f;
}

/* std::clamp(v, lo, hi): con NaN da NaN */
static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (hi < v ? hi : v);
}

int32_t gx_xf_to_int(float f)
{
    if (!(f >= -2147483648.0f && f < 2147483648.0f))
        return INT32_MIN;
    return (int32_t)f;
}

static int clamp255(float v)
{
    const int32_t i = gx_xf_to_int(v);
    return i < 0 ? 0 : (i > 255 ? 255 : i);
}

uint32_t gx_xf_num_texgens(const gx_xf_snap_t *xf)
{
    const uint32_t n = xf->reg[XF_NUMTEXGEN] & 15u;
    return n > 8 ? 8 : n;
}

/* ---- matrices (MultiplyVec*) ------------------------------------------------------- */

static vec3_t mul_v2_m24(vec3_t v, const float *m)
{
    vec3_t r = { m[0] * v.x + m[1] * v.y + m[2] + m[3], m[4] * v.x + m[5] * v.y + m[6] + m[7], 1.0f };
    return r;
}

static vec3_t mul_v2_m34(vec3_t v, const float *m)
{
    vec3_t r = { m[0] * v.x + m[1] * v.y + m[2] + m[3], m[4] * v.x + m[5] * v.y + m[6] + m[7],
                 m[8] * v.x + m[9] * v.y + m[10] + m[11] };
    return r;
}

static vec3_t mul_v3_m33(vec3_t v, const float *m)
{
    vec3_t r = { m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                 m[6] * v.x + m[7] * v.y + m[8] * v.z };
    return r;
}

static vec3_t mul_v3_m24(vec3_t v, const float *m)
{
    vec3_t r = { m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
                 m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7], 1.0f };
    return r;
}

static vec3_t mul_v3_m34(vec3_t v, const float *m)
{
    vec3_t r = { m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
                 m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
                 m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11] };
    return r;
}

/* ---- posición y normales ------------------------------------------------------------ */

static void transform_position(const gx_xf_snap_t *xf, const gx_vtx_in_t *src, gx_vtx_out_t *dst)
{
    const vec3_t p = { src->pos[0], src->pos[1], src->pos[2] };
    const vec3_t mv = mul_v3_m34(p, mat_at(xf, (uint32_t)src->posmtx * 4u));
    dst->mv[0] = mv.x;
    dst->mv[1] = mv.y;
    dst->mv[2] = mv.z;

    float proj[6];
    for (int k = 0; k < 6; k++) {
        const uint32_t w = xf->reg[XF_PROJ + k];
        memcpy(&proj[k], &w, sizeof proj[k]);
    }
    if ((xf->reg[XF_PROJ_TYPE] & 1u) == 0) {                /* perspectiva */
        dst->proj[0] = proj[0] * mv.x + proj[1] * mv.z;
        dst->proj[1] = proj[2] * mv.y + proj[3] * mv.z;
        dst->proj[2] = (proj[4] * mv.z + proj[5]) * (1.0f - (float)1e-7);
        dst->proj[3] = -mv.z;
    } else {                                                /* ortográfica */
        dst->proj[0] = proj[0] * mv.x + proj[1];
        dst->proj[1] = proj[2] * mv.y + proj[3];
        dst->proj[2] = proj[4] * mv.z + proj[5];
        dst->proj[3] = 1;
    }
}

static void transform_normal(const gx_xf_snap_t *xf, const gx_vtx_in_t *src, gx_vtx_out_t *dst)
{
    const float *m = mat_at(xf, XF_NORMAL_MTX + (uint32_t)(src->posmtx & 31u) * 3u);
    for (int k = 0; k < 3; k++) {
        const vec3_t n = { src->nrm[k][0], src->nrm[k][1], src->nrm[k][2] };
        vec3_t r = mul_v3_m33(n, m);
        if (k == 0)                             /* solo la normal: las otras escalan el emboss */
            r = normalized(r);
        dst->nrm[k][0] = r.x;
        dst->nrm[k][1] = r.y;
        dst->nrm[k][2] = r.z;
    }
}

/* ---- luces ---------------------------------------------------------------------------- */

typedef struct light {
    uint32_t color;                             /* rrggbbaa */
    vec3_t   cosatt, distatt, pos, dir;
} light_t;

static light_t get_light(const gx_xf_snap_t *xf, uint32_t i)
{
    const uint32_t b = XF_LIGHTS + 16u * i;
    light_t l;
    l.color = word_u(xf, b + 3);
    l.cosatt = (vec3_t){ word_f(xf, b + 4), word_f(xf, b + 5), word_f(xf, b + 6) };
    l.distatt = (vec3_t){ word_f(xf, b + 7), word_f(xf, b + 8), word_f(xf, b + 9) };
    l.pos = (vec3_t){ word_f(xf, b + 10), word_f(xf, b + 11), word_f(xf, b + 12) };
    l.dir = (vec3_t){ word_f(xf, b + 13), word_f(xf, b + 14), word_f(xf, b + 15) };
    return l;
}

/* Campos de un canal (LitChannel) */
#define CH_MATSRC(c)   ((c) & 1u)
#define CH_LIGHTING(c) (((c) >> 1) & 1u)
#define CH_AMBSRC(c)   (((c) >> 6) & 1u)
#define CH_DIFFUSE(c)  (((c) >> 7) & 3u)
#define CH_ATTN(c)     (((c) >> 9) & 3u)
#define CH_MASK(c)     ((((c) >> 2) & 15u) | ((((c) >> 11) & 15u) << 4))

enum { DIFF_NONE = 0, DIFF_SIGN = 1, DIFF_CLAMP = 2 };
enum { ATTN_NONE = 0, ATTN_SPEC = 1, ATTN_DIR = 2, ATTN_SPOT = 3 };

static inline float safe_divide(float n, float d)
{
    return d == 0 ? (n > 0 ? 1 : 0) : n / d;
}

/* CalculateLightAttn: ldir entra sin normalizar y sale como lo usa la difusa */
static float light_attn(const light_t *l, vec3_t *ldir, vec3_t normal, uint32_t chan)
{
    float attn = 1.0f;
    switch (CH_ATTN(chan)) {
    case ATTN_NONE:
    case ATTN_DIR:
        *ldir = normalized(*ldir);
        if (ldir->x == 0.0f && ldir->y == 0.0f && ldir->z == 0.0f)
            *ldir = normal;
        break;
    case ATTN_SPEC: {
        *ldir = normalized(*ldir);
        attn = dot(*ldir, normal) >= 0.0 ? max0(dot(l->dir, normal)) : 0;
        const vec3_t att_len = { 1.0f, attn, attn * attn };
        vec3_t dist_attn = l->distatt;
        if (CH_DIFFUSE(chan) != DIFF_NONE)
            dist_attn = normalized(dist_attn);
        attn = safe_divide(max0(dot(att_len, l->cosatt)), dot(att_len, dist_attn));
        break;
    }
    case ATTN_SPOT: {
        const float dist2 = dot(*ldir, *ldir);
        const float dist = sqrtf(dist2);
        *ldir = vdiv(*ldir, dist);
        attn = max0(dot(*ldir, l->dir));
        const float cos_att = l->cosatt.x + (l->cosatt.y * attn) + (l->cosatt.z * attn * attn);
        const float dist_att = l->distatt.x + (l->distatt.y * dist) + (l->distatt.z * dist2);
        attn = safe_divide(max0(cos_att), dist_att);
        break;
    }
    }
    return attn;
}

/* LightColor: suma la luz i a light_col (r, g, b) */
static void light_color(const gx_xf_snap_t *xf, vec3_t pos, vec3_t normal, uint32_t i,
                        uint32_t chan, float light_col[3])
{
    const light_t l = get_light(xf, i);
    vec3_t ldir = vsub(l.pos, pos);
    const float attn = light_attn(&l, &ldir, normal, chan);
    float dif = dot(ldir, normal);
    float scale;
    switch (CH_DIFFUSE(chan)) {
    case DIFF_NONE:  scale = attn; break;
    case DIFF_SIGN:  scale = attn * dif; break;
    case DIFF_CLAMP: dif = max0(dif); scale = attn * dif; break;
    default:         return;                    /* Dolphin: PanicAlert */
    }
    /* AddScaledIntegerColor */
    light_col[0] += (float)(l.color >> 24) * scale;
    light_col[1] += (float)((l.color >> 16) & 255u) * scale;
    light_col[2] += (float)((l.color >> 8) & 255u) * scale;
}

/* LightAlpha: el mismo cálculo, pero (alfa * attn) * dif, en ese orden */
static void light_alpha(const gx_xf_snap_t *xf, vec3_t pos, vec3_t normal, uint32_t i,
                        uint32_t chan, float *light_col)
{
    const light_t l = get_light(xf, i);
    vec3_t ldir = vsub(l.pos, pos);
    const float attn = light_attn(&l, &ldir, normal, chan);
    float dif = dot(ldir, normal);
    const float a = (float)(l.color & 255u);
    switch (CH_DIFFUSE(chan)) {
    case DIFF_NONE:  *light_col += a * attn; break;
    case DIFF_SIGN:  *light_col += a * attn * dif; break;
    case DIFF_CLAMP: dif = max0(dif); *light_col += a * attn * dif; break;
    default:         break;
    }
}

static uint8_t lit(uint8_t mat, int light)
{
    return (uint8_t)((mat * (light + (light >> 7))) >> 8);
}

static void transform_color(const gx_xf_snap_t *xf, const gx_vtx_in_t *src, gx_vtx_out_t *dst)
{
    const vec3_t pos = { dst->mv[0], dst->mv[1], dst->mv[2] };
    const vec3_t normal = { dst->nrm[0][0], dst->nrm[0][1], dst->nrm[0][2] };

    for (uint32_t ch = 0; ch < 2; ch++) {
        uint8_t mat[4], out[4];
        const uint32_t matreg = xf->reg[XF_MAT0 + ch], ambreg = xf->reg[XF_AMB0 + ch];

        /* color */
        const uint32_t cc = xf->reg[XF_COLOR0 + ch];
        if (CH_MATSRC(cc)) {
            memcpy(mat, src->col[ch], 4);
        } else {
            mat[0] = (uint8_t)(matreg >> 24);
            mat[1] = (uint8_t)(matreg >> 16);
            mat[2] = (uint8_t)(matreg >> 8);
            mat[3] = (uint8_t)matreg;
        }
        if (CH_LIGHTING(cc)) {
            float lc[3];
            if (CH_AMBSRC(cc)) {
                lc[0] = src->col[ch][0];
                lc[1] = src->col[ch][1];
                lc[2] = src->col[ch][2];
            } else {
                lc[0] = (float)(ambreg >> 24);
                lc[1] = (float)((ambreg >> 16) & 255u);
                lc[2] = (float)((ambreg >> 8) & 255u);
            }
            const uint32_t mask = CH_MASK(cc);
            for (uint32_t i = 0; i < 8; i++)
                if (mask & (1u << i))
                    light_color(xf, pos, normal, i, cc, lc);
            for (int k = 0; k < 3; k++)
                out[k] = lit(mat[k], clamp255(lc[k]));
        } else {
            memcpy(out, mat, 3);
        }

        /* alfa */
        const uint32_t ac = xf->reg[XF_ALPHA0 + ch];
        mat[3] = CH_MATSRC(ac) ? src->col[ch][3] : (uint8_t)(matreg & 255u);
        if (CH_LIGHTING(ac)) {
            float la = CH_AMBSRC(ac) ? (float)src->col[ch][3] : (float)(ambreg & 255u);
            const uint32_t mask = CH_MASK(ac);
            for (uint32_t i = 0; i < 8; i++)
                if (mask & (1u << i))
                    light_alpha(xf, pos, normal, i, ac, &la);
            out[3] = lit(mat[3], clamp255(la));
        } else {
            out[3] = mat[3];
        }
        memcpy(dst->col[ch], out, 4);
    }
}

/* ---- coordenadas de textura ---------------------------------------------------------- */

/* TexMtxInfo */
#define TI_PROJ(t)       (((t) >> 1) & 1u)      /* 0 ST, 1 STQ */
#define TI_INPUT(t)      (((t) >> 2) & 1u)      /* 0 AB11, 1 ABC1 */
#define TI_TYPE(t)       (((t) >> 4) & 7u)
#define TI_ROW(t)        (((t) >> 7) & 31u)
#define TI_EMB_SRC(t)    (((t) >> 12) & 7u)
#define TI_EMB_LIGHT(t)  (((t) >> 15) & 7u)

enum { TG_REGULAR = 0, TG_EMBOSS = 1, TG_COLOR0 = 2, TG_COLOR1 = 3 };
enum { ROW_GEOM = 0, ROW_NORMAL = 1, ROW_COLORS = 2, ROW_BINORMAL_T = 3, ROW_BINORMAL_B = 4, ROW_TEX0 = 5 };

static void texgen_regular(const gx_xf_snap_t *xf, uint32_t info, uint32_t k, const gx_vtx_in_t *src,
                           gx_vtx_out_t *dst)
{
    vec3_t s;
    const uint32_t row = TI_ROW(info);
    switch (row) {
    case ROW_GEOM:       s = (vec3_t){ src->pos[0], src->pos[1], src->pos[2] }; break;
    case ROW_NORMAL:     s = (vec3_t){ src->nrm[0][0], src->nrm[0][1], src->nrm[0][2] }; break;
    case ROW_BINORMAL_T: s = (vec3_t){ src->nrm[1][0], src->nrm[1][1], src->nrm[1][2] }; break;
    case ROW_BINORMAL_B: s = (vec3_t){ src->nrm[2][0], src->nrm[2][1], src->nrm[2][2] }; break;
    default:
        if (row >= ROW_TEX0 && row < ROW_TEX0 + 8)
            s = (vec3_t){ src->tex[row - ROW_TEX0][0], src->tex[row - ROW_TEX0][1], 1.0f };
        else
            s = (vec3_t){ 0.0f, 0.0f, 1.0f };       /* Dolphin: ASSERT y lectura fuera del arreglo */
        break;
    }
    /* Un NaN pasa a 1 (Shadow the Hedgehog, bugs.dolphin-emu.org/issues/11458) */
    if (isnan(s.x))
        s.x = 1;
    if (isnan(s.y))
        s.y = 1;
    if (isnan(s.z))
        s.z = 1;

    const float *m = mat_at(xf, (uint32_t)src->texmtx[k] * 4u);
    vec3_t d;
    if (TI_PROJ(info) == 0)
        d = TI_INPUT(info) == 0 ? mul_v2_m24(s, m) : mul_v3_m24(s, m);
    else
        d = TI_INPUT(info) == 0 ? mul_v2_m34(s, m) : mul_v3_m34(s, m);

    if (xf->reg[XF_DUALTEX] & 1u) {
        const uint32_t post = xf->reg[XF_POSTMTXINFO + k];
        const float *pm = mat_at(xf, XF_POST_MTX + (post & 63u) * 4u);
        const vec3_t t = (post >> 8) & 1u ? normalized(d) : d;
        d = mul_v3_m34(t, pm);
    }

    /* q = 0: el caso especial de la GameCube (neheGX Lesson08, Rogue Squadron 3) */
    if (d.z == 0.0f) {
        d.x = clampf(d.x / 2.0f, -1.0f, 1.0f);
        d.y = clampf(d.y / 2.0f, -1.0f, 1.0f);
    }
    dst->tex[k][0] = d.x;
    dst->tex[k][1] = d.y;
    dst->tex[k][2] = d.z;
}

static void transform_texcoords(const gx_xf_snap_t *xf, const gx_vtx_in_t *src, gx_vtx_out_t *dst)
{
    const uint32_t n = gx_xf_num_texgens(xf);
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t info = xf->reg[XF_TEXMTXINFO + k];
        switch (TI_TYPE(info)) {
        case TG_REGULAR:
            texgen_regular(xf, info, k, src, dst);
            break;
        case TG_EMBOSS: {
            const light_t l = get_light(xf, TI_EMB_LIGHT(info));
            const vec3_t mv = { dst->mv[0], dst->mv[1], dst->mv[2] };
            const vec3_t ldir = normalized(vsub(l.pos, mv));
            const vec3_t t = { dst->nrm[1][0], dst->nrm[1][1], dst->nrm[1][2] };
            const vec3_t b = { dst->nrm[2][0], dst->nrm[2][1], dst->nrm[2][2] };
            const float d1 = dot(ldir, t), d2 = dot(ldir, b);
            const uint32_t from = TI_EMB_SRC(info);
            dst->tex[k][0] = dst->tex[from][0] + d1;
            dst->tex[k][1] = dst->tex[from][1] + d2;
            dst->tex[k][2] = dst->tex[from][2];
            break;
        }
        case TG_COLOR0:
        case TG_COLOR1: {
            const uint8_t *c = dst->col[TI_TYPE(info) - TG_COLOR0];
            dst->tex[k][0] = (float)c[0] / 255.0f;
            dst->tex[k][1] = (float)c[1] / 255.0f;
            dst->tex[k][2] = 1.0f;
            break;
        }
        default:
            break;                              /* Dolphin: "Bad tex gen type" */
        }
    }
    for (uint32_t k = 0; k < n; k++) {
        dst->tex[k][0] *= xf->tex_scale[k][0];
        dst->tex[k][1] *= xf->tex_scale[k][1];
    }
}

void gx_xform(const gx_xf_snap_t *xf, const gx_vtx_in_t *in, uint32_t n, gx_vtx_out_t *out)
{
    for (uint32_t v = 0; v < n; v++) {
        gx_vtx_out_t *o = &out[v];
        memset(o, 0, sizeof *o);
        transform_position(xf, &in[v], o);
        transform_normal(xf, &in[v], o);
        transform_color(xf, &in[v], o);
        transform_texcoords(xf, &in[v], o);
    }
}
