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
 * Decodificadores de texturas (Fase 2, etapa C4), como
 * VideoCommon/TextureDecoder_Common.cpp de Dolphin cf5cb7ef: los 11 formatos
 * (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR) con las 3
 * paletas (IA8, RGB565, RGB5A3), a RGBA de 8 bits.
 *
 * Listos para SPU (D-041): de un búfer a otro, sin estado global ni punteros a
 * la RAM emulada. Quien los llama junta la textura y la paleta. Lo que se lee
 * fuera del búfer da 0, como SafeSpanRead de Dolphin.
 *
 *   gx_tex_decode_texel   un texel; lo usa el muestreador (como Dolphin)
 *   gx_tex_decode         la textura entera, de a bloques (para la etapa D);
 *                         da lo mismo que texel por texel
 */
#ifndef GCPS3_GX_TEXDEC_H
#define GCPS3_GX_TEXDEC_H

#include <stdint.h>

enum {
    GX_TF_I4 = 0x0, GX_TF_I8 = 0x1, GX_TF_IA4 = 0x2, GX_TF_IA8 = 0x3, GX_TF_RGB565 = 0x4,
    GX_TF_RGB5A3 = 0x5, GX_TF_RGBA8 = 0x6, GX_TF_C4 = 0x8, GX_TF_C8 = 0x9, GX_TF_C14X2 = 0xA,
    GX_TF_CMPR = 0xE,
};
enum { GX_TL_IA8 = 0, GX_TL_RGB565 = 1, GX_TL_RGB5A3 = 2 };

#define GX_TMEM_SIZE      (1024u * 1024u)
#define GX_TMEM_LINE_SIZE 32u

/* Un tramo de bytes; leer más allá de n da 0 */
typedef struct gx_span {
    const uint8_t *p;
    uint32_t       n;
} gx_span_t;

/* Formatos que existen (los otros no decodifican nada) */
int gx_tex_valid_format(uint32_t fmt);
/* Tamaño del bloque en texels y del texel en nibbles (TexDecoder_GetBlock*,
 * GetTexelSizeInNibbles); 0 en un formato que no existe */
uint32_t gx_tex_block_width(uint32_t fmt);
uint32_t gx_tex_block_height(uint32_t fmt);
uint32_t gx_tex_nibbles(uint32_t fmt);

/* El texel (s, t) a out en RGBA. width_m1 es el ancho menos 1 (TexImage0).
 * En un formato que no existe out no cambia, como Dolphin. */
void gx_tex_decode_texel(uint8_t out[4], gx_span_t src, int32_t s, int32_t t, int32_t width_m1,
                         uint32_t fmt, gx_span_t tlut, uint32_t tlutfmt);
/* RGBA8 precargado en la TMEM: alfa y rojo en un banco, verde y azul en el otro */
void gx_tex_decode_texel_rgba8_tmem(uint8_t out[4], gx_span_t ar, gx_span_t gb, int32_t s, int32_t t,
                                    int32_t width_m1);

/* La textura entera, width × height texels RGBA en dst, de a bloques */
void gx_tex_decode(uint8_t *dst, gx_span_t src, uint32_t width, uint32_t height, uint32_t fmt,
                   gx_span_t tlut, uint32_t tlutfmt);

#endif
