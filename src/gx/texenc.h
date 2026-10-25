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
 * Codificadores de las copias del EFB (Fase 2, paso C6), listos para SPU: de
 * un búfer a otro, sin estado global; quien los llama junta los datos.
 *
 *   texenc.c   a textura: VideoBackends/Software/TextureEncoder.cpp
 *   xfbenc.c   al XFB: EncodeXFB de VideoBackends/Software/SWEfbInterface.cpp
 *
 * Los dos escriben en el búfer intermedio (el "staging texture" de Dolphin),
 * que después pasa a la RAM fila por fila (copy.c).
 */
#ifndef GCPS3_GX_TEXENC_H
#define GCPS3_GX_TEXENC_H

#include <stdint.h>

/* Píxeles de 24 bits desde el primero de la copia, de corrido como en el EFB
 * de Dolphin (filas de 640; el color y después la Z): los bloques de relleno
 * leen la fila siguiente o el plano siguiente. Lo que pasa de n se lee 0 */
typedef struct gx_pixels {
    const uint32_t *p;
    uint32_t n;
} gx_pixels_t;

typedef struct gx_texenc {
    uint32_t efb_fmt;           /* formato de píxel (BP 0x43): RGB8, RGBA6, RGB565 o Z24 */
    uint32_t copy_fmt;          /* EFBCopyFormat de 4 bits (tp / 2 + (tp & 1) × 8) */
    int      intensity;         /* intensity_fmt y auto_conv: R4, R8, RA4 y RA8 como I */
    int      half;              /* media escala: el promedio de 2 × 2 */
    uint32_t wm1, hm1;          /* BP 0x4A: ancho y alto - 1, sin recortar al EFB */
    uint32_t stride;            /* bytes entre filas de bloques (BP 0x4D << 5) */
} gx_texenc_t;

/* Bytes que toca el codificador desde dst: (filas de bloques - 1) × stride +
 * bloques × bytes por bloque; 0 si Dolphin no escribe nada (formato de píxel
 * 4-7 o un formato que es un PanicAlert) */
uint32_t gx_texenc_extent(const gx_texenc_t *c);

/* EncodeEfbCopy: los bloques del formato de textura, fila de bloques cada stride */
void gx_texenc(uint8_t *dst, gx_pixels_t src, const gx_texenc_t *c);

typedef struct gx_xfbenc {
    uint32_t pf;                /* formato de píxel */
    uint32_t left, top, right, bottom;  /* el rectángulo ya recortado al EFB */
    int      clamp_top, clamp_bottom;
    uint8_t  coef[7];           /* filtro vertical (BP 0x53 y 0x54) */
    uint32_t gamma;             /* BP 0x52, bits 7-8 */
    float    y_scale;
} gx_xfbenc_t;

/* Lo que escribe en dst: ancho × alto escalado × 2 bytes */
uint32_t gx_xfbenc_size(const gx_xfbenc_t *c);

/* EncodeXFB: filtro vertical, gamma, YUV 4:2:2 y escala vertical. source es el
 * búfer de 640 × 528 pares Y/UV que Dolphin guarda entre copias (static):
 * con el rectángulo corrido a la derecha se leen partes de copias anteriores.
 * color es el plano de color del EFB */
void gx_xfbenc(uint8_t *dst, uint8_t *source, const uint32_t *color, const gx_xfbenc_t *c);

#endif
