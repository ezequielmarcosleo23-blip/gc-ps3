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
 * El TEV como fragment program del RSX (Fase 2, etapa D3): el mismo cálculo
 * que tev.c, en fp32 con valores enteros, así que con las mismas entradas da
 * lo mismo salvo la niebla (EX2, LG2 y RCP del RSX).
 *
 * Entradas del programa (los vértices llevan w = 1: todo se interpola lineal
 * en la pantalla, como el rasterizador de GX, y la perspectiva de las
 * coordenadas la hace el programa con S / Q):
 *
 *   TEXCOORDi.xyz   coordenada i: s, t, q por 1/w del vértice
 *   TEXCOORD0-3.w   el color 0 (r, g, b, a) de 0 a 255
 *   TEXCOORD4-7.w   el color 1
 *   FOGC.x          la Z de 0 a 2^24 - 1
 *   WPOS.x          la columna del píxel + 0,5
 *
 * Salidas: R0 el color (de 0 a 1), R1.z la Z si hay textura de Z; KIL si no
 * pasa la prueba de alfa.
 *
 * El programa depende de una parte de BP (la clave, con la escala de las
 * matrices indirectas); lo demás (los colores y las constantes del TEV, los
 * coeficientes de las matrices indirectas, las referencias de la
 * prueba de alfa, la niebla, la escala de las texturas) va en constantes que
 * se escriben en cada dibujo (gx_tevfp_patch).
 */
#ifndef GCPS3_GX_TEVFP_H
#define GCPS3_GX_TEVFP_H

#include <stdint.h>

#include "gx/rsxfp.h"

struct gc_gx;

#define GX_TEVFP_KEY     72
#define GX_TEVFP_WORDS   12288
#define GX_TEVFP_PATCHES 160
#define GX_TEVFP_TEX     20

typedef struct gx_tevfp_key {
    uint32_t v[GX_TEVFP_KEY];
} gx_tevfp_key_t;

/* Lo que se escribe en cada dibujo */
enum {
    GX_TEVFP_P_REG,                             /* arg: registro 0-3 (BP 0xE0-0xE7) */
    GX_TEVFP_P_KONST,                           /* arg: la etapa (su constante) */
    GX_TEVFP_P_INDMTX_S,                        /* arg: matriz 0-2, la columna de s */
    GX_TEVFP_P_INDMTX_T,
    GX_TEVFP_P_TEXSCALE,                        /* arg: texmap, 1 / (128 × ancho y alto) */
    GX_TEVFP_P_ALPHAREF,
    GX_TEVFP_P_ZTEX,                            /* el sesgo en 12 + 12 bits */
    GX_TEVFP_P_FOG,                             /* A, C, B, 2^-corrimiento */
    GX_TEVFP_P_FOGRANGE,                        /* centro - 342, 1 / ancho del viewport */
    GX_TEVFP_P_FOGK,                            /* arg: 0-2, los k × 4 */
    GX_TEVFP_P_FOGCOLOR,
};

/* Lo que Dolphin lee de un píxel anterior (el TEV es un objeto estático): el
 * programa no lo puede repetir */
enum {
    GX_TEVFP_STALE_TEX = 1,                     /* textura sin etapa con textura antes */
    GX_TEVFP_STALE_COLOR = 2,                   /* canal de color que no está activo */
    GX_TEVFP_STALE_IND = 4,                     /* etapa indirecta que no existe, o sin coordenadas */
    GX_TEVFP_STALE_TC = 8,                      /* coordenada sin calcular en este píxel */
};

typedef struct gx_tevfp {
    uint32_t     words[GX_TEVFP_WORDS];         /* orden lógico */
    rsxfp_info_t info;
    uint32_t     npatch;
    struct {
        uint16_t at;                            /* la palabra del primer float */
        uint8_t  kind, arg;
    } patch[GX_TEVFP_PATCHES];
    uint32_t     ntex;                          /* cada TEX, en orden */
    struct {
        uint8_t ind;                            /* 1: etapa indirecta; 0: del TEV */
        uint8_t stage, texmap;
    } tex[GX_TEVFP_TEX];
    uint32_t     stale;
    int          fog, ztex;
} gx_tevfp_t;

/* La parte de BP de la que depende el programa */
void gx_tevfp_key(const uint32_t bp[256], gx_tevfp_key_t *key);
uint32_t gx_tevfp_key_hash(const gx_tevfp_key_t *key);

/* Arma el programa del estado de BP. 0, o el error del ensamblador */
int gx_tevfp_build(const uint32_t bp[256], gx_tevfp_t *p);

/* Escribe las constantes del estado de ahora en words (una copia de
 * p->words). Con raw_coords, las texturas reciben las coordenadas en s17.7 sin
 * escalar (el simulador las pasa así al muestreador del TEV) */
void gx_tevfp_patch(const struct gc_gx *gx, const gx_tevfp_t *p, uint32_t *words, int raw_coords);

#endif
