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
 * Interfaz propia del backend gráfico (BORRADOR).
 *
 * El núcleo GX solo habla con esta interfaz; la implementación en la PS3 puede
 * usar rsxkit o código propio sobre el RSX, y la del runner de PC puede ser
 * nula o volcar trazas. Se fija al terminar la prueba "rsx" de la Fase 0.
 */
#ifndef GCPS3_BACKEND_GFX_BACKEND_H
#define GCPS3_BACKEND_GFX_BACKEND_H

#include <stdint.h>

typedef struct gfx_backend gfx_backend_t;

typedef struct gfx_backend_ops {
    int  (*init)(gfx_backend_t *be, uint32_t width, uint32_t height);
    void (*shutdown)(gfx_backend_t *be);
    void (*begin_frame)(gfx_backend_t *be);
    void (*end_frame)(gfx_backend_t *be);   /* presenta el frame (con RESC en la PS3) */
} gfx_backend_ops_t;

struct gfx_backend {
    const gfx_backend_ops_t *ops;
    void *priv;                             /* estado de la implementación */
};

#endif
