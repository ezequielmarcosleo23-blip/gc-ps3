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
 * El backend de GX (Fase 2, etapa D1): lo que dibuja los triángulos.
 *
 * El frente (FIFO, BP/CP/XF, vértices, XF, armado, recorte, culling, división
 * por w y viewport) es el mismo para todos y da los triángulos que llegan al
 * rasterizador de Dolphin (el volcado GCPS3_TRIANGULOS). El backend los
 * dibuja con el estado de BP, la TMEM y la RAM, y es dueño del EFB.
 *
 * Un backend puede juntar triángulos y dibujarlos después (un lote del RSX).
 * El frente lo vacía (flush) antes de todo lo que cambia lo que lee el
 * dibujo, así que cada triángulo se dibuja con el estado del momento en que
 * llegó:
 *   - cualquier escritura de BP que cambia el valor o que actúa siempre
 *     (copias, cargas de la TMEM, tokens, caja envolvente), salvo la máscara;
 *   - XF del viewport (0x101A-0x101F), que lee la tijera y la niebla de rango;
 *   - al terminar cada tramo de la GPU, antes de que siga la CPU (que puede
 *     cambiar la RAM de las texturas o leer el EFB, la caja o el PE).
 * Las copias, las lecturas del EFB y los volcados piden antes el EFB al día
 * (gc_gx_efb_sync).
 */
#ifndef GCPS3_GX_BACKEND_H
#define GCPS3_GX_BACKEND_H

#include <stdint.h>

#include "gx/efb.h"

struct gc_gx;
struct gx_rvtx;

/* El borrado de una copia (EfbCopy::ClearEfb): el rectángulo ya recortado al
 * EFB, inclusivo, el color (r g b a como gx_efb_set_color), la Z y las
 * máscaras; con any = 0 no se escribe nada */
typedef struct gx_clear {
    uint32_t       left, top, right, bottom;
    uint32_t       color, z;
    gx_efb_write_t w;
    int            any;
} gx_clear_t;

typedef struct gx_backend {
    const char *name;
    /* Su estado en gx->be_state: 0, o -1 sin memoria (puede faltar) */
    int  (*init)(struct gc_gx *gx);
    void (*free)(struct gc_gx *gx);
    /* El encendido (gc_gx_reset), después de que el frente borró el EFB */
    void (*reset)(struct gc_gx *gx);
    /* Un triángulo para dibujar, de frente (Rasterizer::DrawTriangleFrontFace) */
    void (*triangle)(struct gc_gx *gx, const struct gx_rvtx *v0, const struct gx_rvtx *v1,
                     const struct gx_rvtx *v2);
    /* Un triángulo descartado por culling: deja la pendiente de Z para zfreeze
     * (Rasterizer::UpdateZSlope) */
    void (*culled)(struct gc_gx *gx, const struct gx_rvtx *v0, const struct gx_rvtx *v1,
                   const struct gx_rvtx *v2, int32_t x_off, int32_t y_off);
    /* Dibuja lo que tenga juntado (puede faltar) */
    void (*flush)(struct gc_gx *gx);
    /* Deja gx->efb con lo dibujado, después de un flush (puede faltar) */
    void (*efb_sync)(struct gc_gx *gx);
    /* El borrado de una copia, después de un flush */
    void (*clear)(struct gc_gx *gx, const gx_clear_t *c);
} gx_backend_t;

/* El rasterizador por software de la etapa C: dibuja cada triángulo en el acto */
extern const gx_backend_t gx_backend_sw;
/* El mismo, pero junta los triángulos y los dibuja recién al vaciar, como un
 * backend con lotes: sirve para probar que el frente vacía donde hace falta
 * (tiene que dar lo mismo que gx_backend_sw) y para contar los lotes */
extern const gx_backend_t gx_backend_sw_lotes;

/* Lotes de gx_backend_sw_lotes: vaciados con algo adentro y triángulos */
typedef struct gx_lotes_stats {
    uint64_t batches;
    uint64_t triangles;
    uint32_t max;                         /* el lote más grande */
} gx_lotes_stats_t;

int  gx_lotes_stats(const struct gc_gx *gx, gx_lotes_stats_t *out);

#endif
