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
 * El rasterizador (ver raster.h): VideoBackends/Software/Rasterizer.cpp y
 * BPFunctions::ComputeScissorRects de Dolphin cf5cb7ef.
 *
 * Cada triángulo se dibuja una vez por rectángulo de tijera (normalmente
 * uno; más si el desplazamiento de la tijera da la vuelta por 1024). Las
 * aristas van en punto fijo 28.4 con la convención de relleno de Dolphin, se
 * recorre en bloques de 2 × 2 píxeles y los valores (Z, 1/w, colores y
 * coordenadas de textura) se interpolan con pendientes en float, en el mismo
 * orden de cuentas. Las cuentas enteras que en Dolphin podrían desbordar se
 * hacen con la vuelta de 32 bits de x86.
 *
 * Valores que en Dolphin escriben fuera de sus arreglos (más de 2 canales de
 * color, más de 8 coordenadas o más de 4 etapas indirectas en genMode) se
 * recortan a esos máximos: ningún juego los usa y no hay referencia.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gx/gx.h"
#include "gx/raster.h"
#include "gx/tev.h"
#include "gx/x86.h"

enum {
    BP_GENMODE = 0x00, BP_SCISSOR_TL = 0x20, BP_SCISSOR_BR = 0x21, BP_IREF = 0x27,
    BP_TEVORDER = 0x28, BP_ZMODE = 0x40, BP_ZCONTROL = 0x43, BP_SCISSOR_OFFSET = 0x59,
    BP_TEXMODE0 = 0x80, BP_TEXMODE1 = 0x84,
    XF_VIEWPORT_WD = 0x101A, XF_VIEWPORT_HT = 0x101B, XF_VIEWPORT_XORIG = 0x101D,
    XF_VIEWPORT_YORIG = 0x101E,
};

#define BLOCK_SIZE 2

/* ---- aritmética de x86 ------------------------------------------------------------------ */

/* La vuelta de 32 bits (en C desbordar un int es indefinido) */
static inline int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t wmul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

/* >> de un int negativo: GCC lo define aritmético, como x86 */
static inline int32_t sar(int32_t v, unsigned n) { return v >> n; }

static inline int32_t min3(int32_t a, int32_t b, int32_t c)
{
    const int32_t m = a < b ? a : b;
    return m < c ? m : c;
}

static inline int32_t max3(int32_t a, int32_t b, int32_t c)
{
    const int32_t m = a > b ? a : b;
    return m > c ? m : c;
}

/* std::clamp<float> de libstdc++: min(max(v, lo), hi); un NaN pasa */
static inline float clampf(float v, float lo, float hi)
{
    const float m = (v < lo) ? lo : v;
    return (hi < m) ? hi : m;
}

static inline uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

static float xf_f(const gc_gx_t *gx, uint32_t reg)
{
    float f;
    memcpy(&f, &gx->xf[reg], sizeof f);
    return f;
}

/* Lo que cuenta genMode, con los máximos de los arreglos */
static inline uint32_t num_colchans(const gc_gx_t *gx)
{
    const uint32_t n = bits(gx->bp[BP_GENMODE], 4, 3);
    return n < 2 ? n : 2;
}

static inline uint32_t num_texgens(const gc_gx_t *gx)
{
    const uint32_t n = bits(gx->bp[BP_GENMODE], 0, 4);
    return n < 8 ? n : 8;
}

static inline uint32_t num_indstages(const gc_gx_t *gx)
{
    const uint32_t n = bits(gx->bp[BP_GENMODE], 16, 3);
    return n < 4 ? n : 4;
}

/* ---- el triángulo en texto (volcado de GCPS3_TRIANGULOS) --------------------------------- */

static uint32_t fbits(float f)
{
    if (f != f)
        return 0x7fc00000u;
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

void gc_gx_triangle_text(const gx_rvtx_t *const v[3], uint32_t ntex, char buf[GC_GX_TRIANGLE_TEXT])
{
    size_t at = (size_t)snprintf(buf, GC_GX_TRIANGLE_TEXT, "t%u", (unsigned)ntex);
    for (int k = 0; k < 3; k++) {
        at += (size_t)snprintf(buf + at, GC_GX_TRIANGLE_TEXT - at, " v %08x %08x %08x %08x",
                               (unsigned)fbits(v[k]->scr[0]), (unsigned)fbits(v[k]->scr[1]),
                               (unsigned)fbits(v[k]->scr[2]), (unsigned)fbits(v[k]->proj[3]));
        for (int c = 0; c < 2; c++)
            at += (size_t)snprintf(buf + at, GC_GX_TRIANGLE_TEXT - at, " %02x%02x%02x%02x",
                                   v[k]->col[c][0], v[k]->col[c][1], v[k]->col[c][2], v[k]->col[c][3]);
        for (uint32_t i = 0; i < ntex && i < 8; i++)
            at += (size_t)snprintf(buf + at, GC_GX_TRIANGLE_TEXT - at, " %08x %08x %08x",
                                   (unsigned)fbits(v[k]->tex[i][0]), (unsigned)fbits(v[k]->tex[i][1]),
                                   (unsigned)fbits(v[k]->tex[i][2]));
    }
}

/* ---- tijera (BPFunctions::ComputeScissorRects) ------------------------------------------ */

typedef struct range {
    int32_t offset, start, end;
} range_t;

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    const int32_t m = (v < lo) ? lo : v;
    return (hi < m) ? hi : m;
}

/* Los desplazamientos de -4096 a 4096 de a 1024 que dejan algo en el EFB */
static uint32_t scissor_ranges(int32_t start, int32_t end, int32_t offset, int32_t dim, range_t out[3])
{
    uint32_t n = 0;
    for (int32_t extra = -4096; extra <= 4096; extra += 1024) {
        const int32_t off = offset + extra;
        const int32_t s = clampi(start - off, 0, dim);
        const int32_t e = clampi(end - off + 1, 0, dim);
        if (s < e && n < 3) {
            out[n].offset = off;
            out[n].start = s;
            out[n].end = e;
            n++;
        }
    }
    return n;
}

typedef struct vp_bounds {
    int32_t left, right, top, bottom;           /* los float del viewport convertidos a int */
} vp_bounds_t;

/* ScissorResult::GetViewportArea */
static int32_t viewport_area(const vp_bounds_t *vp, const gx_scissor_t *r)
{
    const int32_t x0 = clampi(r->left + r->x_off, vp->left, vp->right);
    const int32_t x1 = clampi(r->right + r->x_off, vp->left, vp->right);
    const int32_t y0 = clampi(r->top + r->y_off, vp->top, vp->bottom);
    const int32_t y1 = clampi(r->bottom + r->y_off, vp->top, vp->bottom);
    return wmul(wsub(x1, x0), wsub(y1, y0));
}

/* ScissorResult::IsWorse: el mejor queda último (se dibuja encima) */
static int is_worse(const vp_bounds_t *vp, const gx_scissor_t *a, const gx_scissor_t *b)
{
    const int32_t aa = viewport_area(vp, a), ba = viewport_area(vp, b);
    if (aa != ba)
        return aa < ba;
    return (a->right - a->left) * (a->bottom - a->top) < (b->right - b->left) * (b->bottom - b->top);
}

static void compute_scissors(const gc_gx_t *gx, gx_raster_t *r)
{
    r->nscissor = 0;
    const uint32_t tl = gx->bp[BP_SCISSOR_TL], br = gx->bp[BP_SCISSOR_BR];
    const int32_t left = (int32_t)bits(tl, 12, 11), top = (int32_t)bits(tl, 0, 11);
    const int32_t right = (int32_t)bits(br, 12, 11), bottom = (int32_t)bits(br, 0, 11);
    if (left > right || top > bottom)
        return;

    /* std::minmax de los bordes del viewport, y a int como en std::clamp<int> */
    const float wd = xf_f(gx, XF_VIEWPORT_WD), ht = xf_f(gx, XF_VIEWPORT_HT);
    const float xo = xf_f(gx, XF_VIEWPORT_XORIG), yo = xf_f(gx, XF_VIEWPORT_YORIG);
    const float x_a = xo - wd, x_b = xo + wd, y_a = yo - ht, y_b = yo + ht;
    vp_bounds_t vp;
    vp.left = gx_x86_f2i(x_b < x_a ? x_b : x_a);
    vp.right = gx_x86_f2i(x_b < x_a ? x_a : x_b);
    vp.top = gx_x86_f2i(y_b < y_a ? y_b : y_a);
    vp.bottom = gx_x86_f2i(y_b < y_a ? y_a : y_b);

    const uint32_t off = gx->bp[BP_SCISSOR_OFFSET];
    range_t xr[3], yr[3];
    const uint32_t nx = scissor_ranges(left, right, (int32_t)bits(off, 0, 9) << 1, GX_EFB_WIDTH, xr);
    const uint32_t ny = scissor_ranges(top, bottom, (int32_t)bits(off, 10, 9) << 1, GX_EFB_HEIGHT, yr);
    for (uint32_t i = 0; i < nx; i++)
        for (uint32_t j = 0; j < ny; j++) {
            gx_scissor_t *s = &r->scissor[r->nscissor++];
            s->left = xr[i].start;
            s->right = xr[i].end;
            s->x_off = xr[i].offset;
            s->top = yr[j].start;
            s->bottom = yr[j].end;
            s->y_off = yr[j].offset;
        }

    /* std::ranges::sort con 16 o menos: inserción, estable */
    for (uint32_t i = 1; i < r->nscissor; i++) {
        const gx_scissor_t v = r->scissor[i];
        uint32_t j = i;
        while (j > 0 && is_worse(&vp, &v, &r->scissor[j - 1])) {
            r->scissor[j] = r->scissor[j - 1];
            j--;
        }
        r->scissor[j] = v;
    }
}

void gx_raster_scissor_changed(gc_gx_t *gx)
{
    compute_scissors(gx, gx->raster);
    gx->raster->scissor_dirty = 0;
}

/* ---- pendientes ------------------------------------------------------------------------- */

typedef struct slope_ctx {
    int32_t x0, y0;
    float   xoff, yoff;
    float   dx10, dx20, dy10, dy20;
} slope_ctx_t;

static slope_ctx_t slope_ctx(const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2,
                             int32_t x0, int32_t y0, int32_t x_off, int32_t y_off)
{
    const float adjust = 0.495f;                /* un poco menos de 0,5 */
    slope_ctx_t c;
    c.x0 = x0;
    c.y0 = y0;
    c.xoff = ((float)x0 - (v0->scr[0] - (float)x_off)) + adjust;
    c.yoff = ((float)y0 - (v0->scr[1] - (float)y_off)) + adjust;
    c.dx10 = v1->scr[0] - v0->scr[0];
    c.dx20 = v2->scr[0] - v0->scr[0];
    c.dy10 = v1->scr[1] - v0->scr[1];
    c.dy20 = v2->scr[1] - v0->scr[1];
    return c;
}

static void slope_set(gx_slope_t *s, float f0, float f1, float f2, const slope_ctx_t *c)
{
    const float d20 = f2 - f0;
    const float d10 = f1 - f0;
    const float a = d20 * c->dy10 - d10 * c->dy20;
    const float b = c->dx20 * d10 - c->dx10 * d20;
    const float cc = c->dx20 * c->dy10 - c->dx10 * c->dy20;
    s->dfdx = a / cc;
    s->dfdy = b / cc;
    s->f0 = f0;
    s->x0 = c->x0;
    s->y0 = c->y0;
    s->xoff = c->xoff;
    s->yoff = c->yoff;
}

static inline float slope_get(const gx_slope_t *s, int32_t x, int32_t y)
{
    const float dx = s->xoff + (float)(x - s->x0);
    const float dy = s->yoff + (float)(y - s->y0);
    return s->f0 + (s->dfdx * dx) + (s->dfdy * dy);
}

/* La pendiente de Z antes del primer triángulo, si zfreeze ya está (Init) */
static void slope_default(gx_slope_t *s)
{
    memset(s, 0, sizeof *s);
    s->f0 = 1.0f;
}

void gx_raster_reset(gx_raster_t *r)
{
    memset(r, 0, sizeof *r);
    slope_default(&r->zslope);
}

/* ---- píxel, bloque y LOD ---------------------------------------------------------------- */

/* log2(f) aproximado en s28.4 */
static int32_t fixed_log2(float f)
{
    uint32_t x;
    memcpy(&x, &f, sizeof x);
    const int32_t log_int = (int32_t)((x & 0x7F800000u) >> 19) - 2032;
    const int32_t log_fract = (int32_t)((x & 0x007FFFFFu) >> 19);
    return log_int + log_fract;
}

/* iround: redondea hacia arriba desde ,5 (con el truncado de (int)) */
static inline int32_t iround(float x)
{
    const int32_t t = gx_x86_f2i(x);
    if ((x - (float)t) >= 0.5f)
        return wadd(t, 1);
    return t;
}

static void calculate_lod(const gc_gx_t *gx, gx_raster_t *r, int32_t *lodp, uint8_t *linear,
                          uint32_t texmap, uint32_t texcoord)
{
    const uint32_t unit = (texmap & 3u) | ((texmap & 4u) << 3);
    const uint32_t tm0 = gx->bp[BP_TEXMODE0 + unit], tm1 = gx->bp[BP_TEXMODE1 + unit];

    const float *uv00 = r->px[0][0].uv[texcoord];
    const float *uv10 = r->px[1][0].uv[texcoord];
    const float *uv01 = r->px[0][1].uv[texcoord];
    const float dudx = fabsf(uv00[0] - uv10[0]);
    const float dvdx = fabsf(uv00[1] - uv10[1]);
    const float dudy = fabsf(uv00[0] - uv01[0]);
    const float dvdy = fabsf(uv00[1] - uv01[1]);

    float sd, td;
    if (bits(tm0, 8, 1)) {                      /* diag_lod */
        sd = dudx + dudy;
        td = dvdx + dvdy;
    } else {
        sd = (dudx < dudy) ? dudy : dudx;       /* std::max */
        td = (dvdx < dvdy) ? dvdy : dvdx;
    }

    int32_t lod = fixed_log2((sd < td) ? td : sd);
    const int32_t bias = (int32_t)(int8_t)bits(tm0, 9, 8);     /* s2.5 */
    lod += sar(bias, 1);

    *linear = (uint8_t)((lod > 0 && bits(tm0, 7, 1)) || (lod <= 0 && bits(tm0, 4, 1)));

    /* El orden de las comparaciones importa */
    const int32_t max_lod = (int32_t)bits(tm1, 8, 8), min_lod = (int32_t)bits(tm1, 0, 8);
    if (lod > max_lod)
        lod = max_lod;
    else if (lod < min_lod)
        lod = min_lod;
    *lodp = lod;
}

static void build_block(const gc_gx_t *gx, gx_raster_t *r, int32_t bx, int32_t by)
{
    const uint32_t ntex = num_texgens(gx);
    for (int32_t yi = 0; yi < BLOCK_SIZE; yi++)
        for (int32_t xi = 0; xi < BLOCK_SIZE; xi++) {
            const int32_t x = xi + bx, y = yi + by;
            const float invw = 1.0f / slope_get(&r->wslope, x, y);
            r->px[xi][yi].invw = invw;
            for (uint32_t i = 0; i < ntex; i++) {
                float projection = invw;
                const float q = slope_get(&r->tslope[i][2], x, y) * invw;
                if (q != 0.0f)
                    projection = invw / q;
                r->px[xi][yi].uv[i][0] = slope_get(&r->tslope[i][0], x, y) * projection;
                r->px[xi][yi].uv[i][1] = slope_get(&r->tslope[i][1], x, y) * projection;
            }
        }

    const uint32_t iref = gx->bp[BP_IREF];
    const uint32_t nind = num_indstages(gx);
    for (uint32_t i = 0; i < nind; i++)
        calculate_lod(gx, r, &r->ind_lod[i], &r->ind_linear[i], bits(iref, 6 * i, 3),
                      bits(iref, 6 * i + 3, 3));

    const uint32_t nstages = bits(gx->bp[BP_GENMODE], 10, 4);
    for (uint32_t i = 0; i <= nstages; i++) {
        const uint32_t order = gx->bp[BP_TEVORDER + (i >> 1)];
        const unsigned sh = (i & 1u) ? 12 : 0;
        if (bits(order, sh + 6, 1))
            calculate_lod(gx, r, &r->tex_lod[i], &r->tex_linear[i], bits(order, sh, 3),
                          bits(order, sh + 3, 3));
    }
}

/* s17.7 en un campo de 24 bits con signo (TextureCoordinateType) */
static inline int32_t sext24(int32_t v)
{
    return (int32_t)((uint32_t)v << 8) >> 8;
}

static void draw(gc_gx_t *gx, gx_raster_t *r, int32_t x, int32_t y, int32_t xi, int32_t yi)
{
    const int32_t z = gx_x86_f2i(clampf(slope_get(&r->zslope, x, y), 0.0f, 16777215.0f));
    const uint32_t zmode = gx->bp[BP_ZMODE];

    /* Z temprana (GetEmulatedZ() == Early) */
    if (bits(zmode, 0, 1) && bits(gx->bp[BP_ZCONTROL], 6, 1)) {
        if (!gx_efb_zcompare(gx->efb, gx->bp, (uint32_t)x, (uint32_t)y, (uint32_t)z))
            return;
    }

    gx_tev_t *t = &r->tev;
    t->pos[0] = x;
    t->pos[1] = y;
    t->pos[2] = z;

    const uint32_t ncol = num_colchans(gx);
    for (uint32_t i = 0; i < ncol; i++)
        for (int c = 0; c < 4; c++)
            t->color[i][c] = (uint8_t)gx_x86_f2i(clampf(slope_get(&r->cslope[i][c], x, y), 0.0f, 255.0f));

    /* × 128 porque el TEV guarda las coordenadas en s17.7 */
    const uint32_t ntex = num_texgens(gx);
    for (uint32_t i = 0; i < ntex; i++) {
        t->uv[i][0] = sext24(gx_x86_f2i(r->px[xi][yi].uv[i][0] * 128.0f));
        t->uv[i][1] = sext24(gx_x86_f2i(r->px[xi][yi].uv[i][1] * 128.0f));
    }

    const uint32_t nind = num_indstages(gx);
    for (uint32_t i = 0; i < nind; i++) {
        t->ind_lod[i] = r->ind_lod[i];
        t->ind_linear[i] = r->ind_linear[i];
    }
    const uint32_t nstages = bits(gx->bp[BP_GENMODE], 10, 4);
    for (uint32_t i = 0; i <= nstages; i++) {
        t->tex_lod[i] = r->tex_lod[i];
        t->tex_linear[i] = r->tex_linear[i];
    }

    gx_tev_draw(gx, t);
}

/* ---- triángulo ---------------------------------------------------------------------------- */

void gx_raster_update_zslope(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1,
                             const gx_rvtx_t *v2, int32_t x_off, int32_t y_off)
{
    if (bits(gx->bp[BP_GENMODE], 19, 1))        /* zfreeze */
        return;
    const int32_t x1 = wsub(iround(16.0f * (v0->scr[0] - (float)x_off)), 9);
    const int32_t y1 = wsub(iround(16.0f * (v0->scr[1] - (float)y_off)), 9);
    const slope_ctx_t c = slope_ctx(v0, v1, v2, sar(wadd(x1, 0xF), 4), sar(wadd(y1, 0xF), 4), x_off, y_off);
    slope_set(&gx->raster->zslope, v0->scr[2], v1->scr[2], v2->scr[2], &c);
}

static void draw_triangle(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2,
                          const gx_scissor_t *sc)
{
    gx_raster_t *r = gx->raster;

    /* La pendiente de Z va antes de la tijera: zfreeze la usa igual */
    gx_raster_update_zslope(gx, v0, v1, v2, sc->x_off, sc->y_off);

    /* 28.4, redondeado y corrido como el hardware */
    const int32_t Y1 = wsub(iround(16.0f * (v0->scr[1] - (float)sc->y_off)), 9);
    const int32_t Y2 = wsub(iround(16.0f * (v1->scr[1] - (float)sc->y_off)), 9);
    const int32_t Y3 = wsub(iround(16.0f * (v2->scr[1] - (float)sc->y_off)), 9);
    const int32_t X1 = wsub(iround(16.0f * (v0->scr[0] - (float)sc->x_off)), 9);
    const int32_t X2 = wsub(iround(16.0f * (v1->scr[0] - (float)sc->x_off)), 9);
    const int32_t X3 = wsub(iround(16.0f * (v2->scr[0] - (float)sc->x_off)), 9);

    const int32_t DX12 = wsub(X1, X2), DX23 = wsub(X2, X3), DX31 = wsub(X3, X1);
    const int32_t DY12 = wsub(Y1, Y2), DY23 = wsub(Y2, Y3), DY31 = wsub(Y3, Y1);
    const int32_t FDX12 = wmul(DX12, 16), FDX23 = wmul(DX23, 16), FDX31 = wmul(DX31, 16);
    const int32_t FDY12 = wmul(DY12, 16), FDY23 = wmul(DY23, 16), FDY31 = wmul(DY31, 16);

    int32_t minx = sar(wadd(min3(X1, X2, X3), 0xF), 4);
    int32_t maxx = sar(wadd(max3(X1, X2, X3), 0xF), 4);
    int32_t miny = sar(wadd(min3(Y1, Y2, Y3), 0xF), 4);
    int32_t maxy = sar(wadd(max3(Y1, Y2, Y3), 0xF), 4);
    if (minx < sc->left)
        minx = sc->left;
    if (maxx > sc->right)
        maxx = sc->right;
    if (miny < sc->top)
        miny = sc->top;
    if (maxy > sc->bottom)
        maxy = sc->bottom;
    if (minx >= maxx || miny >= maxy)
        return;

    /* Las demás pendientes */
    const slope_ctx_t c = slope_ctx(v0, v1, v2, sar(wadd(X1, 0xF), 4), sar(wadd(Y1, 0xF), 4),
                                    sc->x_off, sc->y_off);
    const float w[3] = { 1.0f / v0->proj[3], 1.0f / v1->proj[3], 1.0f / v2->proj[3] };
    slope_set(&r->wslope, w[0], w[1], w[2], &c);
    const uint32_t ncol = num_colchans(gx);
    for (uint32_t i = 0; i < ncol; i++)
        for (int k = 0; k < 4; k++)
            slope_set(&r->cslope[i][k], (float)v0->col[i][k], (float)v1->col[i][k], (float)v2->col[i][k], &c);
    const uint32_t ntex = num_texgens(gx);
    for (uint32_t i = 0; i < ntex; i++)
        for (int k = 0; k < 3; k++)
            slope_set(&r->tslope[i][k], v0->tex[i][k] * w[0], v1->tex[i][k] * w[1], v2->tex[i][k] * w[2], &c);

    /* Constantes de las aristas, con la convención de relleno */
    int32_t C1 = wsub(wmul(DY12, X1), wmul(DX12, Y1));
    int32_t C2 = wsub(wmul(DY23, X2), wmul(DX23, Y2));
    int32_t C3 = wsub(wmul(DY31, X3), wmul(DX31, Y3));
    if (DY12 < 0 || (DY12 == 0 && DX12 > 0))
        C1 = wadd(C1, 1);
    if (DY23 < 0 || (DY23 == 0 && DX23 > 0))
        C2 = wadd(C2, 1);
    if (DY31 < 0 || (DY31 == 0 && DX31 > 0))
        C3 = wadd(C3, 1);

    const int32_t block_minx = minx & ~(BLOCK_SIZE - 1);
    const int32_t block_miny = miny & ~(BLOCK_SIZE - 1);

    for (int32_t y = block_miny; y < maxy; y += BLOCK_SIZE) {
        for (int32_t x = block_minx; x < maxx; x += BLOCK_SIZE) {
            const int32_t x1_ = x + BLOCK_SIZE - 1, y1_ = y + BLOCK_SIZE - 1;
            const int32_t x0 = x * 16, x1 = x1_ * 16, y0 = y * 16, y1 = y1_ * 16;

            /* Las funciones de las aristas en las 4 esquinas */
#define EDGE(C, DX, DY, X, Y) (wsub(wadd(C, wmul(DX, Y)), wmul(DY, X)) > 0)
            const int a = EDGE(C1, DX12, DY12, x0, y0) | EDGE(C1, DX12, DY12, x1, y0) << 1 |
                          EDGE(C1, DX12, DY12, x0, y1) << 2 | EDGE(C1, DX12, DY12, x1, y1) << 3;
            const int b = EDGE(C2, DX23, DY23, x0, y0) | EDGE(C2, DX23, DY23, x1, y0) << 1 |
                          EDGE(C2, DX23, DY23, x0, y1) << 2 | EDGE(C2, DX23, DY23, x1, y1) << 3;
            const int cc = EDGE(C3, DX31, DY31, x0, y0) | EDGE(C3, DX31, DY31, x1, y0) << 1 |
                           EDGE(C3, DX31, DY31, x0, y1) << 2 | EDGE(C3, DX31, DY31, x1, y1) << 3;
#undef EDGE
            if (a == 0 || b == 0 || cc == 0)
                continue;

            build_block(gx, r, x, y);

            /* Cubierto entero: igual hay que mirar la tijera */
            if (a == 0xF && b == 0xF && cc == 0xF && x >= minx && x1_ < maxx && y >= miny && y1_ < maxy) {
                for (int32_t iy = 0; iy < BLOCK_SIZE; iy++)
                    for (int32_t ix = 0; ix < BLOCK_SIZE; ix++)
                        draw(gx, r, x + ix, y + iy, ix, iy);
            } else {
                int32_t CY1 = wsub(wadd(C1, wmul(DX12, y0)), wmul(DY12, x0));
                int32_t CY2 = wsub(wadd(C2, wmul(DX23, y0)), wmul(DY23, x0));
                int32_t CY3 = wsub(wadd(C3, wmul(DX31, y0)), wmul(DY31, x0));
                for (int32_t iy = 0; iy < BLOCK_SIZE; iy++) {
                    int32_t CX1 = CY1, CX2 = CY2, CX3 = CY3;
                    for (int32_t ix = 0; ix < BLOCK_SIZE; ix++) {
                        if (CX1 > 0 && CX2 > 0 && CX3 > 0) {
                            if (x + ix >= minx && x + ix < maxx && y + iy >= miny && y + iy < maxy)
                                draw(gx, r, x + ix, y + iy, ix, iy);
                        }
                        CX1 = wsub(CX1, FDY12);
                        CX2 = wsub(CX2, FDY23);
                        CX3 = wsub(CX3, FDY31);
                    }
                    CY1 = wadd(CY1, FDX12);
                    CY2 = wadd(CY2, FDX23);
                    CY3 = wadd(CY3, FDX31);
                }
            }
        }
    }
}

void gx_raster_triangle(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2)
{
    gx_raster_t *r = gx->raster;
    if (r->scissor_dirty)
        gx_raster_scissor_changed(gx);
    if (r->nscissor == 0)
        return;
    gx_tev_set_konst(gx, &r->tev);
    for (uint32_t i = 0; i < r->nscissor; i++)
        draw_triangle(gx, v0, v1, v2, &r->scissor[i]);
}
