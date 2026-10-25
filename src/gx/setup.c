/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2009 Dolphin Emulator Project
 * Partes basadas en el trabajo de Markus Trenkwalder, Copyright (c) 2007, 2008
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
 * Armado y recorte (Fase 2, etapa C): VideoCommon/IndexGenerator.cpp,
 * VideoBackends/Software/SetupUnit.cpp y Clipper.cpp de Dolphin cf5cb7ef.
 *
 * Los vértices llegan transformados en el orden de carga y se arman con el
 * orden de IndexGenerator (sin "primitive restart"):
 *
 *   triángulos      (0 1 2) (3 4 5) ...
 *   tira            (0 1 2) (1 3 2) (2 3 4) (3 5 4) ...
 *   abanico         (0 1 2) (0 2 3) ...
 *   cuadrados       (0 1 2) (0 2 3), y si sobran 3, un triángulo
 *   líneas          (0 1) (2 3) ...;  tira de líneas (0 1) (1 2) ...
 *
 * Cada triángulo se descarta si queda entero del mismo lado de un plano;
 * después, culling (los descartados igual dejan la pendiente de Z para
 * zfreeze), recorte contra los 6 planos en coordenadas homogéneas, división
 * por w y viewport. Las líneas y los puntos se dibujan como dos triángulos con
 * el ancho de BP 0x22. Las cuentas de float siguen el orden de Dolphin. Los
 * triángulos que quedan van al backend (backend.h).
 *
 * La licencia original de Clipper.cpp (BSD de 3 cláusulas, de Markus
 * Trenkwalder) es compatible con la GPL.
 */
#include <math.h>
#include <string.h>

#include "gx/gx.h"
#include "gx/raster.h"

enum {
    PRIM_QUADS = 0, PRIM_QUADS_2 = 1, PRIM_TRIANGLES = 2, PRIM_STRIP = 3, PRIM_FAN = 4,
    PRIM_LINES = 5, PRIM_LINE_STRIP = 6, PRIM_POINTS = 7,
};

enum { CULL_NONE = 0, CULL_BACK = 1, CULL_FRONT = 2, CULL_ALL = 3 };

enum {
    SKIP = -1,
    CLIP_POS_X = 0x01, CLIP_NEG_X = 0x02, CLIP_POS_Y = 0x04, CLIP_NEG_Y = 0x08,
    CLIP_POS_Z = 0x10, CLIP_NEG_Z = 0x20,
};

/* XF y BP */
enum {
    XF_CLIP_DISABLE = 0x1005, XF_VIEWPORT = 0x101A, XF_NUMTEXGEN = 0x103F,
    BP_GENMODE = 0x00, BP_LINEPT = 0x22, BP_SU_SSIZE = 0x30, BP_SCISSOR_OFFSET = 0x59,
};

static const float line_pt_tex_offsets[8] = { 0, 1 / 16.f, 1 / 8.f, 1 / 4.f, 1 / 2.f, 1, 1, 1 };

static float xf_f(const gc_gx_t *gx, uint32_t reg)
{
    float f;
    memcpy(&f, &gx->xf[reg], sizeof f);
    return f;
}

/* >> de un int negativo como en x86 (aritmético) */
static inline int32_t asr(int32_t v, unsigned n)
{
    return v >= 0 ? v >> n : -(int32_t)(((uint32_t)-(int64_t)v + ((1u << n) - 1u)) >> n);
}

/* ---- recorte (Clipper.cpp) ------------------------------------------------------------ */

static int clip_mask(const gx_rvtx_t *v)
{
    const float x = v->proj[0], y = v->proj[1], z = v->proj[2], w = v->proj[3];
    int m = 0;
    if (w - x < 0)
        m |= CLIP_POS_X;
    if (x + w < 0)
        m |= CLIP_NEG_X;
    if (w - y < 0)
        m |= CLIP_POS_Y;
    if (y + w < 0)
        m |= CLIP_NEG_Y;
    if (w * z > 0)
        m |= CLIP_POS_Z;
    if (z + w < 0)
        m |= CLIP_NEG_Z;
    return m;
}

/* OutputVertexData::Lerp: a + (b - a) × t; los colores con t en 1/256 */
static void lerp(gx_rvtx_t *d, float t, const gx_rvtx_t *a, const gx_rvtx_t *b)
{
    for (int k = 0; k < 4; k++)
        d->proj[k] = a->proj[k] + ((b->proj[k] - a->proj[k]) * t);
    const int32_t t_int = (int32_t)(uint16_t)gx_xf_to_int(t * 256);
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < 4; i++)
            d->col[c][i] = (uint8_t)(a->col[c][i] + asr(((int32_t)b->col[c][i] - a->col[c][i]) * t_int, 8));
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 3; k++)
            d->tex[i][k] = a->tex[i][k] + ((b->tex[i][k] - a->tex[i][k]) * t);
}

static void add_interpolated(gx_raster_t *r, float t, int out, int in, int *num)
{
    lerp(r->verts[(*num)++], t, r->verts[out], r->verts[in]);
}

static float clip_dot(const gx_raster_t *r, int i, float a, float b, float c, float d)
{
    const gx_rvtx_t *v = r->verts[i];
    return v->proj[0] * a + v->proj[1] * b + v->proj[2] * c + v->proj[3] * d;
}

static int different_signs(float x, float y)
{
    return (x <= 0 && y > 0) || (x > 0 && y <= 0);
}

typedef struct plane {
    int   bit;
    float a, b, c, d;
} plane_t;

static const plane_t poly_planes[6] = {
    { CLIP_POS_X, -1, 0, 0, 1 }, { CLIP_NEG_X, 1, 0, 0, 1 }, { CLIP_POS_Y, 0, -1, 0, 1 },
    { CLIP_NEG_Y, 0, 1, 0, 1 },  { CLIP_POS_Z, 0, 0, 0, 1 }, { CLIP_NEG_Z, 0, 0, 1, 1 },
};

static const plane_t line_planes[6] = {
    { CLIP_POS_X, -1, 0, 0, 1 }, { CLIP_NEG_X, 1, 0, 0, 1 }, { CLIP_POS_Y, 0, -1, 0, 1 },
    { CLIP_NEG_Y, 0, 1, 0, 1 },  { CLIP_POS_Z, 0, 0, -1, 1 }, { CLIP_NEG_Z, 0, 0, 1, 1 },
};

/* ClipTriangle: indices sale con los triángulos (SKIP si no queda nada) */
static void clip_triangle(gx_raster_t *r, int *indices, int *num_indices)
{
    const int mask = clip_mask(r->verts[0]) | clip_mask(r->verts[1]) | clip_mask(r->verts[2]);
    if (mask == 0)
        return;

    int vlist[2][2 * 6 + 1];
    int *inlist = vlist[0], *outlist = vlist[1];
    int n = 3, num = 3;
    inlist[0] = 0;
    inlist[1] = 1;
    inlist[2] = 2;
    indices[0] = indices[1] = indices[2] = SKIP;

    for (int p = 0; p < 6; p++) {
        const plane_t *pl = &poly_planes[p];
        if (!(mask & pl->bit))
            continue;
        int idx_prev = inlist[0];
        float dp_prev = clip_dot(r, idx_prev, pl->a, pl->b, pl->c, pl->d);
        int outcount = 0;
        inlist[n] = inlist[0];
        for (int j = 1; j <= n; j++) {
            const int idx = inlist[j];
            const float dp = clip_dot(r, idx, pl->a, pl->b, pl->c, pl->d);
            if (dp_prev >= 0)
                outlist[outcount++] = idx_prev;
            if (different_signs(dp, dp_prev)) {
                if (dp < 0)
                    add_interpolated(r, dp / (dp - dp_prev), idx, idx_prev, &num);
                else
                    add_interpolated(r, dp_prev / (dp_prev - dp), idx_prev, idx, &num);
                outlist[outcount++] = num - 1;
            }
            idx_prev = idx;
            dp_prev = dp;
        }
        if (outcount < 3)
            return;                                 /* el "continue" de POLY_CLIP */
        int *tmp = inlist;
        inlist = outlist;
        outlist = tmp;
        n = outcount;
    }

    indices[0] = inlist[0];
    indices[1] = inlist[1];
    indices[2] = inlist[2];
    for (int j = 3; j < n; ++j) {
        indices[(*num_indices)++] = inlist[0];
        indices[(*num_indices)++] = inlist[j - 1];
        indices[(*num_indices)++] = inlist[j];
    }
}

static void clip_line(gx_raster_t *r, int *indices)
{
    int cm[2] = { clip_mask(r->verts[0]), clip_mask(r->verts[1]) };
    const int mask = cm[0] | cm[1];
    if (mask == 0)
        return;

    float t0 = 0, t1 = 0;
    indices[0] = indices[1] = SKIP;
    for (int p = 0; p < 6; p++) {
        const plane_t *pl = &line_planes[p];
        if (!(mask & pl->bit))
            continue;
        const float dp0 = clip_dot(r, 0, pl->a, pl->b, pl->c, pl->d);
        const float dp1 = clip_dot(r, 1, pl->a, pl->b, pl->c, pl->d);
        const int neg0 = dp0 < 0, neg1 = dp1 < 0;
        if (neg0 && neg1)
            return;
        if (neg1) {
            const float t = dp1 / (dp1 - dp0);
            if (t > t1)
                t1 = t;
        } else if (neg0) {
            const float t = dp0 / (dp0 - dp1);
            if (t > t0)
                t0 = t;
        }
    }

    indices[0] = 0;
    indices[1] = 1;
    int num = 2;
    if (cm[0]) {
        indices[0] = num;
        add_interpolated(r, t0, 0, 1, &num);
    }
    if (cm[1]) {
        indices[1] = num;
        add_interpolated(r, t1, 1, 0, &num);
    }
}

/* ---- división por w, cara de atrás ------------------------------------------------------ */

static void perspective_divide(const gc_gx_t *gx, gx_rvtx_t *v)
{
    const float w_inv = 1.0f / v->proj[3];
    v->scr[0] = v->proj[0] * w_inv * xf_f(gx, XF_VIEWPORT + 0) + xf_f(gx, XF_VIEWPORT + 3);
    v->scr[1] = v->proj[1] * w_inv * xf_f(gx, XF_VIEWPORT + 1) + xf_f(gx, XF_VIEWPORT + 4);
    v->scr[2] = v->proj[2] * w_inv * xf_f(gx, XF_VIEWPORT + 2) + xf_f(gx, XF_VIEWPORT + 5);
}

static int is_backface(const gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2)
{
    const float x0 = v0->proj[0], x1 = v1->proj[0], x2 = v2->proj[0];
    const float y1 = v1->proj[1], y0 = v0->proj[1], y2 = v2->proj[1];
    const float w0 = v0->proj[3], w1 = v1->proj[3], w2 = v2->proj[3];
    const float normal_z = (x0 * w2 - x2 * w0) * y1 + (x2 * y0 - x0 * y2) * w1 + (y2 * w0 - y0 * w2) * x1;
    int back = normal_z <= 0.0f;
    if (xf_f(gx, XF_VIEWPORT + 1) > 0)             /* viewport positivo: la prueba al revés */
        back = !back;
    return back;
}

/* ---- al backend ----------------------------------------------------------------------- */

/* Un triángulo que llega al rasterizador: el volcado, la cuenta y el backend */
static void emit(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2)
{
    if (gx->triangle_cb) {
        const gx_rvtx_t *const v[3] = { v0, v1, v2 };
        gx->triangle_cb(gx->triangle_ctx, v, gx->bp[BP_GENMODE] & 15u);
    }
    gx->triangles++;
    gx->be->triangle(gx, v0, v1, v2);
}

/* ---- triángulos, líneas y puntos ------------------------------------------------------ */

static void process_triangle(gc_gx_t *gx, gx_rvtx_t *v0, gx_rvtx_t *v1, gx_rvtx_t *v2)
{
    gx_raster_t *r = gx->raster;
    if (clip_mask(v0) & clip_mask(v1) & clip_mask(v2))
        return;                                     /* afuera del todo: ni la pendiente de Z */

    const int back = is_backface(gx, v0, v1, v2);
    const uint32_t cull = (gx->bp[BP_GENMODE] >> 14) & 3u;
    const int32_t xo = (int32_t)((gx->bp[BP_SCISSOR_OFFSET] & 0x1FFu) * 2);
    const int32_t yo = (int32_t)(((gx->bp[BP_SCISSOR_OFFSET] >> 10) & 0x1FFu) * 2);
    if (!back) {
        if (cull == CULL_BACK || cull == CULL_ALL) {
            perspective_divide(gx, v0);
            perspective_divide(gx, v1);
            perspective_divide(gx, v2);
            gx->be->culled(gx, v0, v1, v2, xo, yo);
            return;
        }
    } else if (cull == CULL_FRONT || cull == CULL_ALL) {
        perspective_divide(gx, v0);
        perspective_divide(gx, v2);
        perspective_divide(gx, v1);
        gx->be->culled(gx, v0, v2, v1, xo, yo);
        return;
    }

    int indices[GX_CLIP_VERTICES + 3];
    for (int k = 0; k < GX_CLIP_VERTICES + 3; k++)
        indices[k] = k < 3 ? k : SKIP;
    int num_indices = 3;
    r->verts[0] = v0;
    r->verts[1] = back ? v2 : v1;
    r->verts[2] = back ? v1 : v2;

    /* Sin detección de recorte (XF 0x1005 bit 0) se recorta igual si algún w es negativo */
    int skip_clipping = 0;
    if ((gx->xf[XF_CLIP_DISABLE] & 1u) && r->verts[0]->proj[3] >= 0 && r->verts[1]->proj[3] >= 0 &&
        r->verts[2]->proj[3] >= 0)
        skip_clipping = 1;
    if (!skip_clipping)
        clip_triangle(r, indices, &num_indices);

    for (int i = 0; i + 3 <= num_indices; i += 3) {
        if (indices[i] == SKIP)
            continue;
        gx_rvtx_t *a = r->verts[indices[i]], *b = r->verts[indices[i + 1]], *c = r->verts[indices[i + 2]];
        perspective_divide(gx, a);
        perspective_divide(gx, b);
        perspective_divide(gx, c);
        emit(gx, a, b, c);
    }
}

/* Coordenadas que llevan el desplazamiento de líneas y puntos: las de XF, hasta 8 */
static uint32_t num_texgens(const gc_gx_t *gx)
{
    const uint32_t n = gx->xf[XF_NUMTEXGEN] & 15u;
    return n > 8 ? 8 : n;
}

static void copy_line_vertex(const gc_gx_t *gx, gx_rvtx_t *dst, const gx_rvtx_t *src, int px, int py,
                             int apply_offset)
{
    const uint32_t lp = gx->bp[BP_LINEPT];
    const float half = (float)(lp & 0xFFu) / 12.0f;
    memcpy(dst->proj, src->proj, sizeof dst->proj);
    dst->scr[0] = src->scr[0] + (float)px * half;
    dst->scr[1] = src->scr[1] + (float)py * half;
    dst->scr[2] = src->scr[2];
    memcpy(dst->col, src->col, sizeof dst->col);
    memcpy(dst->tex, src->tex, sizeof dst->tex);

    const float off = line_pt_tex_offsets[(lp >> 16) & 7u];
    if (apply_offset && off != 0) {
        for (uint32_t k = 0; k < num_texgens(gx); k++) {
            const uint32_t s = gx->bp[BP_SU_SSIZE + 2 * k];
            if ((s >> 18) & 1u)
                dst->tex[k][0] += (float)((s & 0xFFFFu) + 1u) * off;
        }
    }
}

static void process_line(gc_gx_t *gx, gx_rvtx_t *l0, gx_rvtx_t *l1)
{
    gx_raster_t *r = gx->raster;
    int indices[4] = { 0, 1, SKIP, SKIP };
    r->verts[0] = l0;
    r->verts[1] = l1;
    r->verts[2] = &r->clipped[17];                  /* donde guardar si se recorta */
    clip_line(r, indices);
    if (indices[0] == SKIP)
        return;

    gx_rvtx_t *v0 = r->verts[indices[0]], *v1 = r->verts[indices[1]];
    perspective_divide(gx, v0);
    perspective_divide(gx, v1);
    const float dx = v1->scr[0] - v0->scr[0], dy = v1->scr[1] - v0->scr[1];

    /* Los extremos son verticales u horizontales según la pendiente; px y py
     * con el signo que deja los triángulos en sentido antihorario */
    int px = 0, py = 0;
    if (fabsf(dx) > fabsf(dy))
        py = dx > 0 ? -1 : 1;
    else
        px = dy > 0 ? 1 : -1;

    gx_rvtx_t t[3];
    memset(t, 0, sizeof t);
    copy_line_vertex(gx, &t[0], v0, px, py, 0);
    copy_line_vertex(gx, &t[1], v1, px, py, 0);
    copy_line_vertex(gx, &t[2], v1, -px, -py, 1);
    emit(gx, &t[2], &t[1], &t[0]);
    copy_line_vertex(gx, &t[1], v0, -px, -py, 1);
    emit(gx, &t[0], &t[1], &t[2]);
}

static void copy_point_vertex(const gc_gx_t *gx, gx_rvtx_t *dst, const gx_rvtx_t *src, int px, int py)
{
    const uint32_t lp = gx->bp[BP_LINEPT];
    const float radius = (float)((lp >> 8) & 0xFFu) / 12.0f;
    memcpy(dst->proj, src->proj, sizeof dst->proj);
    dst->scr[0] = src->scr[0] + (float)(px ? 1 : -1) * radius;
    dst->scr[1] = src->scr[1] + (float)(py ? 1 : -1) * radius;
    dst->scr[2] = src->scr[2];
    memcpy(dst->col, src->col, sizeof dst->col);
    memcpy(dst->tex, src->tex, sizeof dst->tex);

    const float off = line_pt_tex_offsets[(lp >> 19) & 7u];
    if (off != 0) {
        for (uint32_t k = 0; k < num_texgens(gx); k++) {
            const uint32_t s = gx->bp[BP_SU_SSIZE + 2 * k], t = gx->bp[BP_SU_SSIZE + 2 * k + 1];
            if ((s >> 19) & 1u) {
                if (px)
                    dst->tex[k][0] += (float)((s & 0xFFFFu) + 1u) * off;
                if (py)
                    dst->tex[k][1] += (float)((t & 0xFFFFu) + 1u) * off;
            }
        }
    }
}

static void process_point(gc_gx_t *gx, gx_rvtx_t *center)
{
    perspective_divide(gx, center);                 /* Dolphin no recorta los puntos */
    gx_rvtx_t ll, lr, ul, ur;
    copy_point_vertex(gx, &ll, center, 0, 0);
    copy_point_vertex(gx, &lr, center, 1, 0);
    copy_point_vertex(gx, &ur, center, 1, 1);
    copy_point_vertex(gx, &ul, center, 0, 1);
    emit(gx, &ll, &ul, &lr);
    emit(gx, &ur, &lr, &ul);
}

/* ---- armado --------------------------------------------------------------------------- */

void gx_setup_begin(gc_gx_t *gx, uint32_t type)
{
    gc_gx_bbox_flush(gx);                       /* SWVertexLoader::DrawCurrentBatch */
    gx_raster_t *r = gx->raster;
    r->type = type;
    r->count = 0;
    for (int i = 0; i < GX_CLIP_VERTICES; i++)
        r->verts[i + 3] = &r->clipped[i];
}

void gx_setup_vertex(gc_gx_t *gx, const gx_vtx_out_t *o)
{
    gx_raster_t *r = gx->raster;
    gx_rvtx_t v;
    memcpy(v.proj, o->proj, sizeof v.proj);
    memset(v.scr, 0, sizeof v.scr);
    memcpy(v.col, o->col, sizeof v.col);
    memcpy(v.tex, o->tex, sizeof v.tex);
    gx_rvtx_t *w = r->win;

    switch (r->type) {
    case PRIM_TRIANGLES:
        w[r->count++] = v;
        if (r->count == 3) {
            process_triangle(gx, &w[0], &w[1], &w[2]);
            r->count = 0;
        }
        return;
    case PRIM_QUADS:
    case PRIM_QUADS_2:
        w[r->count++] = v;
        if (r->count == 3) {
            process_triangle(gx, &w[0], &w[1], &w[2]);
        } else if (r->count == 4) {
            process_triangle(gx, &w[0], &w[2], &w[3]);
            r->count = 0;
        }
        return;
    case PRIM_STRIP:
        if (r->count < 2) {
            w[r->count++] = v;
            return;
        }
        w[2] = v;
        if ((r->count & 1u) == 0)                   /* i par: (i-2, i-1, i) */
            process_triangle(gx, &w[0], &w[1], &w[2]);
        else                                        /* i impar: (i-2, i, i-1) */
            process_triangle(gx, &w[0], &w[2], &w[1]);
        w[0] = w[1];
        w[1] = w[2];
        r->count++;
        return;
    case PRIM_FAN:
        if (r->count < 2) {
            w[r->count++] = v;
            return;
        }
        w[2] = v;
        process_triangle(gx, &w[0], &w[1], &w[2]);
        w[1] = w[2];
        r->count++;
        return;
    case PRIM_LINES:
        w[r->count++] = v;
        if (r->count == 2) {
            process_line(gx, &w[0], &w[1]);
            r->count = 0;
        }
        return;
    case PRIM_LINE_STRIP:
        if (r->count == 0) {
            w[0] = v;
            r->count = 1;
            return;
        }
        w[1] = v;
        process_line(gx, &w[0], &w[1]);
        w[0] = w[1];
        return;
    default:                                        /* puntos */
        w[0] = v;
        process_point(gx, &w[0]);
        return;
    }
}
