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
 * Los backends por software (ver backend.h): el rasterizador de la etapa C
 * (raster.c, tev.c, efb.c) dibujando en el acto, y el mismo con lotes.
 */
#include <stdlib.h>
#include <string.h>

#include "gx/backend.h"
#include "gx/gx.h"
#include "gx/raster.h"

/* ---- en el acto ----------------------------------------------------------------------- */

static void sw_triangle(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2)
{
    gx_raster_triangle(gx, v0, v1, v2);
}

static void sw_culled(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2,
                      int32_t x_off, int32_t y_off)
{
    gx_raster_update_zslope(gx, v0, v1, v2, x_off, y_off);
}

static void sw_clear(gc_gx_t *gx, const gx_clear_t *c)
{
    if (c->any)
        for (uint32_t y = c->top; y <= c->bottom; y++)
            for (uint32_t x = c->left; x <= c->right; x++) {
                gx_efb_set_color(gx->efb, &c->w, x, y, c->color);
                gx_efb_set_depth(gx->efb, &c->w, x, y, c->z);
            }
    gx_raster_scissor_changed(gx);              /* "Scissor rect must be restored" */
}

const gx_backend_t gx_backend_sw = {
    .name = "software",
    .triangle = sw_triangle,
    .culled = sw_culled,
    .clear = sw_clear,
};

/* ---- con lotes ------------------------------------------------------------------------ */

#define LOTE_MAX 1024                           /* entradas; si se llena, se dibuja */

typedef struct lote_entry {
    int32_t   culled;                           /* 0: dibujar; 1: solo la pendiente de Z */
    int32_t   x_off, y_off;
    gx_rvtx_t v[3];
} lote_entry_t;

typedef struct lote {
    uint32_t         n, triangles;
    gx_lotes_stats_t stats;
    lote_entry_t     e[LOTE_MAX];
} lote_t;

static void lotes_flush(gc_gx_t *gx)
{
    lote_t *l = gx->be_state;
    if (l->n == 0)
        return;
    for (uint32_t i = 0; i < l->n; i++) {
        const lote_entry_t *e = &l->e[i];
        if (e->culled)
            gx_raster_update_zslope(gx, &e->v[0], &e->v[1], &e->v[2], e->x_off, e->y_off);
        else
            gx_raster_triangle(gx, &e->v[0], &e->v[1], &e->v[2]);
    }
    if (l->triangles) {
        l->stats.batches++;
        l->stats.triangles += l->triangles;
        if (l->triangles > l->stats.max)
            l->stats.max = l->triangles;
    }
    l->n = 0;
    l->triangles = 0;
}

static lote_entry_t *lotes_push(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2)
{
    lote_t *l = gx->be_state;
    if (l->n == LOTE_MAX)
        lotes_flush(gx);                        /* el estado no cambió: da lo mismo */
    lote_entry_t *e = &l->e[l->n++];
    e->v[0] = *v0;
    e->v[1] = *v1;
    e->v[2] = *v2;
    return e;
}

static void lotes_triangle(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2)
{
    lote_entry_t *e = lotes_push(gx, v0, v1, v2);
    e->culled = 0;
    e->x_off = e->y_off = 0;
    ((lote_t *)gx->be_state)->triangles++;
}

static void lotes_culled(gc_gx_t *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2,
                         int32_t x_off, int32_t y_off)
{
    lote_entry_t *e = lotes_push(gx, v0, v1, v2);
    e->culled = 1;
    e->x_off = x_off;
    e->y_off = y_off;
}

static int lotes_init(gc_gx_t *gx)
{
    gx->be_state = calloc(1, sizeof(lote_t));
    return gx->be_state ? 0 : -1;
}

static void lotes_free(gc_gx_t *gx)
{
    free(gx->be_state);
    gx->be_state = NULL;
}

static void lotes_reset(gc_gx_t *gx)
{
    lote_t *l = gx->be_state;
    l->n = l->triangles = 0;
    memset(&l->stats, 0, sizeof l->stats);
}

const gx_backend_t gx_backend_sw_lotes = {
    .name = "software con lotes",
    .init = lotes_init,
    .free = lotes_free,
    .reset = lotes_reset,
    .triangle = lotes_triangle,
    .culled = lotes_culled,
    .flush = lotes_flush,
    .clear = sw_clear,
};

int gx_lotes_stats(const gc_gx_t *gx, gx_lotes_stats_t *out)
{
    if (gx->be != &gx_backend_sw_lotes || !gx->be_state)
        return -1;
    *out = ((const lote_t *)gx->be_state)->stats;
    return 0;
}

/* ---- elegir --------------------------------------------------------------------------- */

int gc_gx_set_backend(gc_gx_t *gx, const gx_backend_t *be)
{
    if (gx->be) {
        gc_gx_flush(gx);
        if (gx->be->free)
            gx->be->free(gx);
    }
    gx->be = &gx_backend_sw;
    gx->be_state = NULL;
    if (be->init && be->init(gx) != 0)
        return -1;
    gx->be = be;
    if (be->reset)
        be->reset(gx);
    return 0;
}
