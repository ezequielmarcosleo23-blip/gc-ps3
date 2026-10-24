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
 * Del vértice transformado al píxel (Fase 2, etapa C), como el renderizador
 * por software de Dolphin (commit cf5cb7ef):
 *
 *   setup.c   triángulos, líneas y puntos con el orden de IndexGenerator y
 *             SetupUnit; recorte contra el volumen de vista, culling, división
 *             por w y viewport (Clipper.cpp)
 *   raster.c  el rasterizador (Rasterizer.cpp): tijera, pendientes, bloques
 *             de 2 × 2 píxeles y LOD
 *   tev.c     el TEV y la mezcla con el EFB (Tev.cpp, SWEfbInterface.cpp)
 *
 * Corre en el PPU y en la PC (D-044): es la referencia exacta hasta que el RSX
 * dibuje en la etapa D.
 */
#ifndef GCPS3_GX_RASTER_H
#define GCPS3_GX_RASTER_H

#include <stdint.h>

#include "gx/tev.h"
#include "gx/xform.h"

struct gc_gx;

/* Un vértice en camino al rasterizador (OutputVertexData sin la posición en
 * la vista ni las normales, que ya no se usan) */
typedef struct gx_rvtx {
    float   proj[4];                    /* x, y, z, w proyectados */
    float   scr[3];                     /* en la pantalla, después de dividir por w */
    uint8_t col[2][4];                  /* r g b a */
    float   tex[8][3];
} gx_rvtx_t;

#define GX_CLIP_VERTICES 33             /* NUM_CLIPPED_VERTICES de Clipper.cpp */
#define GX_SCISSOR_MAX   9              /* hasta 3 rangos en x por 3 en y */

/* Un valor interpolado en la pantalla (Slope de Rasterizer.cpp) */
typedef struct gx_slope {
    float   dfdx, dfdy, f0;
    int32_t x0, y0;
    float   xoff, yoff;
} gx_slope_t;

/* Un rectángulo de tijera [left, right) × [top, bottom) con su
 * desplazamiento (BPFunctions::ScissorRect) */
typedef struct gx_scissor {
    int32_t left, top, right, bottom;
    int32_t x_off, y_off;
} gx_scissor_t;

/* El estado de setup.c, raster.c y tev.c (en el montón: unos 7 KB) */
typedef struct gx_raster {
    /* Armado de la primitiva: los vértices que todavía hacen falta */
    uint32_t   type;                    /* tipo de primitiva (bits 3-5 del opcode) */
    uint32_t   count;                   /* vértices recibidos */
    gx_rvtx_t  win[4];
    /* Recorte */
    gx_rvtx_t  clipped[GX_CLIP_VERTICES];
    gx_rvtx_t *verts[GX_CLIP_VERTICES + 3];

    /* Rasterizador: pendientes del triángulo en curso (la de Z queda para
     * zfreeze), los rectángulos de tijera y el bloque de 2 × 2 */
    gx_slope_t   zslope, wslope, cslope[2][4], tslope[8][3];
    gx_scissor_t scissor[GX_SCISSOR_MAX];
    uint32_t     nscissor;
    int          scissor_dirty;         /* BP 0x20, 0x21, 0x59 o el viewport cambiaron */
    struct {
        float invw;
        float uv[8][2];
    } px[2][2];                         /* [xi][yi] */
    int32_t      ind_lod[4], tex_lod[16];
    uint8_t      ind_linear[4], tex_linear[16];

    gx_tev_t     tev;
} gx_raster_t;

/* Estado de encendido del rasterizador (Rasterizer::Init y el TEV en cero) */
void gx_raster_reset(gx_raster_t *r);

/* Empieza una primitiva (SetupUnit::Init) */
void gx_setup_begin(struct gc_gx *gx, uint32_t type);
/* Un vértice transformado más, en el orden de carga */
void gx_setup_vertex(struct gc_gx *gx, const gx_vtx_out_t *v);

/* Un triángulo listo para rasterizar (Rasterizer::DrawTriangleFrontFace) */
void gx_raster_triangle(struct gc_gx *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1, const gx_rvtx_t *v2);
/* Recalcula los rectángulos de tijera con BP y el viewport de ahora
 * (Rasterizer::ScissorChanged). Al dibujar se recalculan solos si BP 0x20,
 * 0x21, 0x59 o el viewport cambiaron; el borrado de una copia los recalcula
 * enseguida, como Dolphin. */
void gx_raster_scissor_changed(struct gc_gx *gx);

/* La pendiente de Z para zfreeze, también con los triángulos descartados */
void gx_raster_update_zslope(struct gc_gx *gx, const gx_rvtx_t *v0, const gx_rvtx_t *v1,
                             const gx_rvtx_t *v2, int32_t x_off, int32_t y_off);

#define GC_GX_TRIANGULOS_VERSION 1
#define GC_GX_TRIANGLE_TEXT      1024

/* Un triángulo en texto, como el parche de Dolphin (GCPS3_TRIANGULOS):
 * "t<n>" y por vértice "v" x, y, z de la pantalla, w, los 2 colores y las n
 * coordenadas, con los bits de cada float (NaN como 7fc00000) */
void gc_gx_triangle_text(const gx_rvtx_t *const v[3], uint32_t ntex, char buf[GC_GX_TRIANGLE_TEXT]);

#endif
