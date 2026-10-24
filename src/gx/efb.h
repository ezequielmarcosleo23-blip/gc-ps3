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
 * El EFB (Fase 2, etapa C): 640 × 528 píxeles de color y de Z, cada uno de 24
 * bits empaquetados según el formato de píxel de BP 0x43, como los guarda el
 * renderizador por software de Dolphin (SWEfbInterface.cpp, commit cf5cb7ef):
 *
 *   RGB8_Z24 y Z24   rrrrrrrr gggggggg bbbbbbbb
 *   RGBA6_Z24        rrrrrr gggggg bbbbbb aaaaaa
 *   RGB565_Z16       como RGB8 (Dolphin todavía no lo emula aparte)
 *
 * Los colores van como 0xRRGGBBAA (el u8[4] "abgr" de Dolphin leído en x86).
 */
#ifndef GCPS3_GX_EFB_H
#define GCPS3_GX_EFB_H

#include <stdint.h>

#define GX_EFB_WIDTH  640
#define GX_EFB_HEIGHT 528

enum {
    GX_PF_RGB8_Z24 = 0, GX_PF_RGBA6_Z24 = 1, GX_PF_RGB565_Z16 = 2, GX_PF_Z24 = 3,
    GX_PF_Y8 = 4, GX_PF_U8 = 5, GX_PF_V8 = 6, GX_PF_YUV420 = 7,
};

typedef struct gx_efb {
    uint32_t color[GX_EFB_WIDTH * GX_EFB_HEIGHT];   /* 24 bits por píxel */
    uint32_t depth[GX_EFB_WIDTH * GX_EFB_HEIGHT];
} gx_efb_t;

/* Lo que se escribe: la máscara de BP 0x41 (color y alfa) y de 0x40 (Z) */
typedef struct gx_efb_write {
    uint32_t pf;                        /* formato de píxel (BP 0x43, bits 0-2) */
    int      color_update, alpha_update, z_update;
} gx_efb_write_t;

/* SetPixelAlphaColor, SetPixelColorOnly, SetPixelAlphaOnly según la máscara */
void     gx_efb_set_color(gx_efb_t *e, const gx_efb_write_t *w, uint32_t x, uint32_t y, uint32_t rgba);
void     gx_efb_set_depth(gx_efb_t *e, const gx_efb_write_t *w, uint32_t x, uint32_t y, uint32_t z);
/* GetPixelColor: 0xRRGGBBAA (sin alfa, 0xFF); 0 en un formato que no existe */
uint32_t gx_efb_get_color(const gx_efb_t *e, uint32_t pf, uint32_t x, uint32_t y);
uint32_t gx_efb_get_depth(const gx_efb_t *e, uint32_t pf, uint32_t x, uint32_t y);

/* CRC-32 de [x0, x1) × [y0, y1): los 3 bytes de cada color (el menos
 * significativo primero, como en la memoria de Dolphin), fila por fila, y
 * aparte los de Z (el volcado de GCPS3_COPIAS) */
void     gx_efb_crc(const gx_efb_t *e, int x0, int y0, int x1, int y1, uint32_t *color, uint32_t *depth);

#endif
