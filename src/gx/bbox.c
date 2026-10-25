/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2014 Dolphin Emulator Project
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
 * La caja envolvente (bounding box) como Dolphin cf5cb7ef con BBoxEnable
 * prendido y el renderizador por software (VideoCommon/BoundingBox.cpp y
 * VideoBackends/Software/SWBoundingBox.cpp): dos capas, lo que escribe BP y
 * lee el PE (con su caché y sus valores pendientes) y las coordenadas que
 * actualiza cada píxel dibujado. Los juegos que la usan (Paper Mario) escriben
 * 1023, 0, 1023, 0 antes de dibujar y la leen después de GXDrawDone.
 *
 * En Dolphin un lote se dibuja recién cuando se vacía: si el CPU leyera la
 * caja sin esperar a la GPU, Dolphin no vería los píxeles del lote pendiente y
 * gc-ps3 sí. Con GXDrawDone (lo que hace el SDK) es igual.
 */
#include "gx/gx.h"

void gc_gx_bbox_set(gc_gx_t *gx, uint32_t index, uint16_t value)
{
    gx->bbox.active = 1;
    if (gx->bbox.valid && gx->bbox.value[index] == value)
        return;
    gx->bbox.value[index] = value;
    gx->bbox.dirty[index] = 1;
}

void gc_gx_bbox_flush(gc_gx_t *gx)
{
    if (!gx->bbox.active)
        return;
    gx->bbox.valid = 0;
    for (int i = 0; i < 4; i++)
        if (gx->bbox.dirty[i]) {
            gx->bbox.coord[i] = (uint16_t)gx->bbox.value[i];
            gx->bbox.dirty[i] = 0;
        }
}

void gc_gx_bbox_update(gc_gx_t *gx, uint32_t x, uint32_t y)
{
    uint16_t *c = gx->bbox.coord;
    const uint16_t left = (uint16_t)(x & ~1u), right = (uint16_t)(x | 1u);
    const uint16_t top = (uint16_t)(y & ~1u), bottom = (uint16_t)(y | 1u);
    if (left < c[0])
        c[0] = left;
    if (right > c[1])
        c[1] = right;
    if (top < c[2])
        c[2] = top;
    if (bottom > c[3])
        c[3] = bottom;
}

uint16_t gc_gx_bbox_get(gc_gx_t *gx, uint32_t index)
{
    gc_gx_flush(gx);                            /* la CPU ya la vació al seguir; por las dudas */
    gx->bbox.active = 0;
    if (!gx->bbox.valid) {
        /* Readback: los valores pendientes se quedan */
        for (int i = 0; i < 4; i++)
            if (!gx->bbox.dirty[i])
                gx->bbox.value[i] = gx->bbox.coord[i];
        gx->bbox.valid = 1;
    }
    return (uint16_t)gx->bbox.value[index];
}
