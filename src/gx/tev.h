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
 * El TEV y lo que sigue hasta el EFB (Fase 2, etapa C), como
 * VideoBackends/Software/Tev.cpp de Dolphin cf5cb7ef: etapas indirectas,
 * combinadores, prueba de alfa, textura de Z, niebla, Z tardía y mezcla.
 *
 * En Dolphin el TEV es un objeto estático: lo que no se vuelve a calcular en
 * un píxel (el color de un canal que no está activo, la textura de una etapa
 * sin textura, ...) queda del píxel anterior, aunque sea de otro triángulo.
 * gx_tev_t guarda todo eso y vuelve a cero solo con gc_gx_reset.
 */
#ifndef GCPS3_GX_TEV_H
#define GCPS3_GX_TEV_H

#include <stdint.h>

struct gc_gx;

/* Los componentes de TevColor (el orden de Dolphin: a, b, g, r) */
enum { GX_ALP = 0, GX_BLU = 1, GX_GRN = 2, GX_RED = 3 };

typedef struct gx_tev {
    int16_t  reg[4][4];             /* PREV, C0, C1, C2 */
    int16_t  konst[4][4];           /* KonstantColors */
    int16_t  raw_tex[4], tex[4], ras[4], stage_konst[4];
    uint8_t  alpha_bump;
    uint8_t  ind_tex[4][4];         /* el texel de cada etapa indirecta, RGBA */
    int32_t  tc_s, tc_t;            /* TexCoord: s17.7 en 24 bits */

    /* Lo que deja el rasterizador para cada píxel */
    int32_t  pos[3];                /* x, y, z */
    uint8_t  color[2][4];           /* RGBA */
    int32_t  uv[8][2];              /* s17.7 en 24 bits */
    int32_t  ind_lod[4], tex_lod[16];
    uint8_t  ind_linear[4], tex_linear[16];
} gx_tev_t;

/* Las constantes K0-K3 de BP 0xE0-0xE7 (Tev::SetKonstColors, al dibujar) */
void gx_tev_set_konst(struct gc_gx *gx, gx_tev_t *t);

/* Un píxel (Tev::Draw): combina, prueba alfa, niebla, Z tardía y lo mezcla
 * con el EFB */
void gx_tev_draw(struct gc_gx *gx, gx_tev_t *t);

/* Una muestra de la textura texmap en s, t (s17.7) con el LOD dado: RGBA.
 * Las texturas llegan en el paso C4; hasta entonces da negro. */
void gx_tex_sample(struct gc_gx *gx, int32_t s, int32_t t, int32_t lod, int linear, uint32_t texmap,
                   uint8_t out[4]);

#endif
