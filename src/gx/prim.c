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
 * Primitivas (Fase 2, etapa B): de los vértices del FIFO a vértices
 * transformados, de a bloques de GX_VTX_BLOCK (D-043). Lo que hace Dolphin con
 * el renderizador por software (commit cf5cb7ef), repartido en
 * VertexLoaderManager::RunVertices, VertexManagerBase (Flush, CalculateNormals),
 * IndexGenerator y SWVertexLoader::DrawCurrentBatch:
 *
 *   - las primitivas de triángulos, cuadrados, líneas y puntos se parten de a
 *     16 380 vértices; cada parte se carga entera (los descartados por índice
 *     de posición no cuentan) y la caché de normales queda con las del último
 *     vértice: normal y tangente si no se descartó, binormal la del último
 *     cargado (un CMP del cargador x64 la guarda siempre);
 *   - solo se transforman los vértices que usa la primitiva: todos en tiras,
 *     abanicos, tiras de líneas y puntos (si alcanzan para una), 3 × n/3 en
 *     triángulos, 2 × n/2 en líneas, y en cuadrados los de cada 4 más un
 *     triángulo si sobran 3. Siempre son los primeros, en el orden de carga;
 *   - si XF y BP no coinciden en cantidad de coordenadas o de canales de
 *     color, Dolphin no dibuja (VertexManagerBase::Flush) y acá no se
 *     transforma, aunque la caché se actualiza igual.
 *
 * Dolphin transforma al vaciar el lote, con el estado de ese momento; acá se
 * transforma al leer la primitiva, que es lo que hace el hardware. Da lo mismo
 * porque todo lo que cambia el XF vacía el lote antes, salvo una escritura de
 * XF 0x1018/0x1019 con el mismo valor que ya tenía el registro de CP
 * (docs/fase2.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/gx.h"
#include "gx/vtx.h"
#include "gx/xform.h"

#define LOG_MAX 16
#define RUN_MAX 16380u                  /* "divisible by both 4 and 3" */

enum {
    PRIM_QUADS = 0, PRIM_QUADS_2 = 1, PRIM_TRIANGLES = 2, PRIM_STRIP = 3, PRIM_FAN = 4,
    PRIM_LINES = 5, PRIM_LINE_STRIP = 6, PRIM_POINTS = 7,
};

/* XF y BP que se usan acá */
enum { XF_NUMCHAN = 0x1009, XF_MATINDEX_A = 0x1018, XF_MATINDEX_B = 0x1019, XF_NUMTEXGEN = 0x103F };
enum { BP_GENMODE = 0x00, BP_SU_SSIZE = 0x30 };

struct gc_gx_vtx {
    gx_xf_snap_t      snap;
    gx_vtx_plan_t     plan[8];
    gx_vtx_defaults_t def;              /* nrm: la caché de normales */
    uint8_t           gathered[GX_VTX_BLOCK * GX_VTX_DIRECT];
    gx_vtx_in_t       in[GX_VTX_BLOCK];
    gx_vtx_out_t      out[GX_VTX_BLOCK];
};

int gc_gx_vtx_init(gc_gx_t *gx)
{
    gx->vtx = malloc(sizeof *gx->vtx);
    if (!gx->vtx)
        return -1;
    gc_gx_vtx_reset(gx);
    return 0;
}

void gc_gx_vtx_free(gc_gx_t *gx)
{
    free(gx->vtx);
    gx->vtx = NULL;
}

void gc_gx_vtx_reset(gc_gx_t *gx)
{
    if (gx->vtx)
        memset(gx->vtx, 0, sizeof *gx->vtx);
    memset(gx->vtx_plan_ok, 0, sizeof gx->vtx_plan_ok);
    gx->xf_dirty = 1;
    gx->drawn = 0;
}

/* ---- texto del volcado ------------------------------------------------------------- */

static uint32_t fbits(float f)
{
    if (f != f)
        return 0x7fc00000u;
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

void gc_gx_vertex_text(const gx_vtx_out_t *v, uint32_t ntex, char buf[GC_GX_VERTEX_TEXT])
{
    size_t at = 0;
    at += (size_t)snprintf(buf + at, GC_GX_VERTEX_TEXT - at, "v %08x %08x %08x p %08x %08x %08x %08x n",
                           (unsigned)fbits(v->mv[0]), (unsigned)fbits(v->mv[1]),
                           (unsigned)fbits(v->mv[2]), (unsigned)fbits(v->proj[0]),
                           (unsigned)fbits(v->proj[1]), (unsigned)fbits(v->proj[2]),
                           (unsigned)fbits(v->proj[3]));
    for (int k = 0; k < 3; k++)
        at += (size_t)snprintf(buf + at, GC_GX_VERTEX_TEXT - at, " %08x %08x %08x",
                               (unsigned)fbits(v->nrm[k][0]), (unsigned)fbits(v->nrm[k][1]),
                               (unsigned)fbits(v->nrm[k][2]));
    at += (size_t)snprintf(buf + at, GC_GX_VERTEX_TEXT - at, " c");
    for (int k = 0; k < 2; k++)
        at += (size_t)snprintf(buf + at, GC_GX_VERTEX_TEXT - at, " %02x%02x%02x%02x", v->col[k][0],
                               v->col[k][1], v->col[k][2], v->col[k][3]);
    at += (size_t)snprintf(buf + at, GC_GX_VERTEX_TEXT - at, " t%u", (unsigned)ntex);
    for (uint32_t k = 0; k < ntex && k < 8; k++)
        at += (size_t)snprintf(buf + at, GC_GX_VERTEX_TEXT - at, " %08x %08x %08x",
                               (unsigned)fbits(v->tex[k][0]), (unsigned)fbits(v->tex[k][1]),
                               (unsigned)fbits(v->tex[k][2]));
}

/* ---- una primitiva ------------------------------------------------------------------ */

/* Los vértices que usa una primitiva de n cargados (IndexGenerator sin
 * "primitive restart", que el renderizador por software no tiene) */
static uint32_t referenced(uint32_t type, uint32_t n)
{
    switch (type) {
    case PRIM_QUADS:
    case PRIM_QUADS_2:     return n % 4 == 3 ? n : n / 4 * 4;
    case PRIM_TRIANGLES:   return n / 3 * 3;
    case PRIM_STRIP:
    case PRIM_FAN:         return n >= 3 ? n : 0;
    case PRIM_LINES:       return n / 2 * 2;
    case PRIM_LINE_STRIP:  return n >= 2 ? n : 0;
    default:               return n;            /* puntos */
    }
}

static const uint8_t *ram_fn(void *ctx, uint32_t addr, uint32_t len)
{
    return gc_ram_phys(&((gc_gx_t *)ctx)->sys->mem, addr, len);
}

/* La foto del XF: su memoria, sus registros y las escalas de BP SU_SSIZE/TSIZE */
static void snapshot(gc_gx_t *gx)
{
    gx_xf_snap_t *s = &gx->vtx->snap;
    memcpy(s->mem, gx->xf, sizeof s->mem);
    memcpy(s->reg, gx->xf + GC_XF_REGS_START, sizeof s->reg);
    for (int k = 0; k < 8; k++) {
        s->tex_scale[k][0] = (float)((gx->bp[BP_SU_SSIZE + 2 * k] & 0xFFFFu) + 1u);
        s->tex_scale[k][1] = (float)((gx->bp[BP_SU_SSIZE + 2 * k + 1] & 0xFFFFu) + 1u);
    }
    gx->xf_dirty = 0;
}

/* Una parte de la primitiva: n vértices del FIFO */
static void run(gc_gx_t *gx, const gx_vtx_plan_t *plan, uint32_t type, const uint8_t *raw,
                uint32_t n, int draw)
{
    struct gc_gx_vtx *v = gx->vtx;
    const uint32_t loaded = gx_vtx_count_loaded(plan, raw, n);
    const uint32_t use = draw ? referenced(type, loaded) : 0;
    const uint32_t ntex = gx->xf[XF_NUMTEXGEN] & 15u;
    uint32_t done = 0, bad = 0, last_m = 0;

    /* La caché solo cambia con la normal en el formato: sin ella no hace falta
     * decodificar lo que no se usa */
    const uint32_t need = plan->has_nrm ? loaded : use;
    for (uint32_t at = 0; at < n && done < need; at += GX_VTX_BLOCK) {
        const uint32_t chunk = n - at < GX_VTX_BLOCK ? n - at : GX_VTX_BLOCK;
        const uint32_t m = gx_vtx_gather(plan, gx->cp.array_base, gx->cp.array_stride, ram_fn, gx,
                                         raw + (size_t)at * plan->raw_size, chunk, v->gathered, &bad);
        if (m == 0)
            continue;
        gx_vtx_decode(&plan->desc, &v->def, v->gathered, m, v->in);
        const uint32_t x = done < use ? (use - done < m ? use - done : m) : 0;
        if (x) {
            gx_xform(&v->snap, v->in, x, v->out);
            if (gx->vertex_cb)
                for (uint32_t k = 0; k < x; k++)
                    gx->vertex_cb(gx->vertex_ctx, &v->out[k], ntex);
            gx->drawn += x;
        }
        done += m;
        last_m = m;
        /* La binormal: la del último cargado (con NTB no se lee de la caché) */
        if (plan->has_ntb)
            memcpy(v->def.nrm[2], v->in[m - 1].nrm[2], sizeof v->def.nrm[2]);
    }

    /* Normal y tangente: las del último vértice, si no se descartó; entonces es
     * el último que se decodificó */
    if (plan->has_nrm && last_m > 0) {
        const uint8_t *last = raw + (size_t)(n - 1) * plan->raw_size;
        if (gx_vtx_count_loaded(plan, last, 1) == 1) {
            const gx_vtx_in_t *l = &v->in[last_m - 1];
            memcpy(v->def.nrm[0], l->nrm[0], sizeof v->def.nrm[0]);
            if (plan->has_ntb)
                memcpy(v->def.nrm[1], l->nrm[1], sizeof v->def.nrm[1]);
        }
    }

    if (bad && gx->warnings++ < LOG_MAX)
        GC_LOGW("gx: %u lecturas de arreglos de vertices fuera de la RAM", (unsigned)bad);
}

void gc_gx_draw(gc_gx_t *gx, uint8_t op, const uint8_t *data, uint32_t n, uint32_t vsize)
{
    struct gc_gx_vtx *v = gx->vtx;
    if (n == 0 || !v)
        return;
    const uint32_t vat = op & 7u, type = (op >> 3) & 7u;

    gx_vtx_plan_t *plan = &v->plan[vat];
    if (!gx->vtx_plan_ok[vat]) {
        const uint32_t vcd[2] = { gx->cp.vcd_lo, gx->cp.vcd_hi };
        gx_vtx_setup(vcd, gx->cp.vat[vat], plan);
        gx->vtx_plan_ok[vat] = 1;
    }
    if (plan->raw_size != vsize) {               /* un formato de color inválido */
        if (gx->warnings++ < LOG_MAX)
            GC_LOGW("gx: formato de vertice %u invalido, la primitiva no se carga", (unsigned)vat);
        return;
    }

    /* Lo que no está en el vértice: índices de matriz de XF (no los de CP) */
    const uint32_t ma = gx->xf[XF_MATINDEX_A], mb = gx->xf[XF_MATINDEX_B];
    v->def.posmtx = (uint8_t)(ma & 0x3Fu);
    for (unsigned i = 0; i < 4; i++) {
        v->def.texmtx[i] = (uint8_t)((ma >> (6 + 6 * i)) & 0x3Fu);
        v->def.texmtx[4 + i] = (uint8_t)((mb >> (6 * i)) & 0x3Fu);
    }

    const uint32_t gen = gx->bp[BP_GENMODE];
    const int draw = (gx->xf[XF_NUMTEXGEN] & 15u) == (gen & 15u) &&
                     (gx->xf[XF_NUMCHAN] & 3u) == ((gen >> 4) & 7u);
    if (draw && gx->xf_dirty)
        snapshot(gx);

    const int split = type == PRIM_QUADS || type == PRIM_QUADS_2 || type == PRIM_TRIANGLES ||
                      type == PRIM_LINES || type == PRIM_POINTS;
    for (uint32_t at = 0; at < n;) {
        const uint32_t k = split && n - at > RUN_MAX ? RUN_MAX : n - at;
        run(gx, plan, type, data + (size_t)at * vsize, k, draw);
        at += k;
    }
}
