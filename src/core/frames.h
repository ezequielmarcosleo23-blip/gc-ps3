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
 * Los cuadros que va mostrando la VI: al terminar cada campo con XFB se arma
 * el cuadro RGB (core/xfb.h) y, si su CRC-32 cambió, se avisa. Como el
 * momento lo fija la emulación (el fin del campo), el runner y la app de PS3
 * ven la misma lista de cuadros para los mismos ciclos: así se comparan.
 */
#ifndef GCPS3_CORE_FRAMES_H
#define GCPS3_CORE_FRAMES_H

#include <stddef.h>
#include <stdint.h>

struct gc_system;

typedef struct gc_frame {
    uint32_t       number;            /* cuadros distintos vistos, desde 1 */
    uint64_t       field;             /* campos de la VI al armarlo (gc_vi_t.fields) */
    uint32_t       width, height;
    uint32_t       crc;               /* CRC-32 de los bytes RGB */
    const uint8_t *rgb;               /* width x height x 3 */
} gc_frame_t;

typedef void (*gc_frame_fn)(void *ctx, const gc_frame_t *frame);

typedef struct gc_frames {
    struct gc_system *sys;
    uint8_t    *rgb;
    size_t      cap;
    gc_frame_t  last;                 /* el último avisado (number 0: ninguno) */
    uint32_t    too_big;              /* cuadros que no entraron en el búfer */
    gc_frame_fn fn;
    void       *ctx;
} gc_frames_t;

/* Se engancha a la VI de sys (usa vi.on_field). rgb es el búfer del cuadro:
 * 640 x 480 x 3 alcanza para 480i; 720 x 576 x 3 para cualquier modo normal. */
void gc_frames_attach(gc_frames_t *f, struct gc_system *sys, uint8_t *rgb, size_t cap,
                      gc_frame_fn fn, void *ctx);

/* Vuelve a empezar la lista (al arrancar otro programa). */
void gc_frames_reset(gc_frames_t *f);

#endif
