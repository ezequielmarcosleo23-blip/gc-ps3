/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008 Dolphin Emulator Project
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
 * Cargador de vértices (Fase 2, etapa B), en dos partes (D-041, D-043):
 *
 *   1. Juntar (PPU, gx_vtx_gather): recorre los vértices del FIFO y reemplaza
 *      cada atributo indexado por los bytes de su arreglo en la RAM; quedan
 *      vértices "directos" de tamaño fijo en un búfer. Una posición con índice
 *      0xFF (8 bits) o 0xFFFF (16 bits) descarta el vértice.
 *   2. Decodificar (listo para SPU, gx_vtx_decode): de ese búfer a vértices en
 *      float, de a bloques de hasta GX_VTX_BLOCK, sin estado global ni
 *      punteros a la RAM emulada.
 *
 * La semántica es la del cargador x64 de Dolphin (VertexLoaderX64.cpp, commit
 * cf5cb7ef), el que usa por omisión y por lo tanto la referencia:
 *   - posición y coordenadas enteras se multiplican por 2^-frac solo si el VAT
 *     tiene ByteDequant (bit 30 del grupo A); las normales enteras, siempre,
 *     por 2^-7, 2^-6, 2^-15 o 2^-14 (u8, s8, u16, s16); los float no cambian;
 *   - XY deja z en 0, S deja t en 0;
 *   - el índice de matriz de posición se queda con 6 bits; los de textura, con
 *     los 8 (el cargador en C de Dolphin los recorta a 6);
 *   - los colores pasan a RGBA de 8 bits (565, 888, 888x, 4444, 6666, 8888);
 *     los que no tienen alfa la dejan en 255;
 * y la del renderizador por software al leerlos (SWVertexLoader.cpp): sin
 * color, 255 en todo; si solo está el color 1, va al canal 0; sin normal,
 * tangente o binormal, las últimas que se cargaron (la "caché"); sin índices
 * de matriz, los de XF (MatrixIndexA y B, no los de CP).
 */
#ifndef GCPS3_GX_VTX_H
#define GCPS3_GX_VTX_H

#include <stdint.h>

#define GX_VTX_BLOCK  64        /* vértices por bloque (D-043) */
#define GX_VTX_DIRECT 160       /* bytes máximos de un vértice juntado: 1 + 8 + 12 + 36 + 8 + 64 */

/* Un vértice decodificado: lo que el XF recibe (InputVertexData de Dolphin) */
typedef struct gx_vtx_in {
    float   pos[3];
    float   nrm[3][3];          /* normal, tangente, binormal */
    float   tex[8][2];
    uint8_t col[2][4];          /* r g b a */
    uint8_t posmtx;
    uint8_t texmtx[8];
    uint8_t pad[7];
} gx_vtx_in_t;

/* Lo que no está en el vértice y se toma de afuera */
typedef struct gx_vtx_defaults {
    float   nrm[3][3];          /* la caché: últimas normal, tangente y binormal cargadas */
    uint8_t posmtx;             /* XF MatrixIndexA */
    uint8_t texmtx[8];          /* XF MatrixIndexA y B */
} gx_vtx_defaults_t;

/* Un atributo en el vértice juntado: desplazamiento (-1 si no está),
 * formato del componente o del color, cuántos componentes y la escala. */
typedef struct gx_vtx_attr {
    int16_t off;
    uint8_t fmt;
    uint8_t count;
    float   scale;              /* 0: sin escala (float, o entero sin ByteDequant) */
} gx_vtx_attr_t;

/* Cómo decodificar un vértice juntado (todo directo) */
typedef struct gx_vtx_desc {
    uint16_t      stride;       /* bytes por vértice juntado */
    int16_t       posmtx;       /* desplazamiento del byte, o -1 */
    int16_t       texmtx[8];
    gx_vtx_attr_t pos;          /* count 2 o 3 */
    gx_vtx_attr_t nrm;          /* count 3 (N) o 9 (NTB) */
    gx_vtx_attr_t col[2];       /* fmt: formato de color */
    gx_vtx_attr_t tex[8];       /* count 1 o 2 */
} gx_vtx_desc_t;

/* Un paso para juntar: un atributo del vértice del FIFO */
enum { GX_VTX_NONE = 0, GX_VTX_DIRECT_ATTR = 1, GX_VTX_INDEX8 = 2, GX_VTX_INDEX16 = 3 };

typedef struct gx_vtx_step {
    uint8_t  type;              /* directo, índice de 8 o de 16 bits */
    uint8_t  array;             /* arreglo de CP (0 posición, 1 normal, 2-3 color, 4-11 textura) */
    uint8_t  parts;             /* 3 con NormalIndex3: tres índices, un tercio de entrada cada uno */
    uint8_t  is_pos;            /* posición: su índice máximo descarta el vértice */
    uint16_t bytes;             /* bytes del atributo en el vértice juntado */
} gx_vtx_step_t;

#define GX_VTX_STEPS 21     /* 9 índices de matriz, posición, normal, 2 colores, 8 coordenadas */

typedef struct gx_vtx_plan {
    gx_vtx_step_t step[GX_VTX_STEPS];
    uint32_t      nsteps;
    uint32_t      raw_size;     /* bytes de un vértice en el FIFO */
    gx_vtx_desc_t desc;
    uint8_t       has_nrm, has_ntb;   /* para actualizar la caché */
    uint8_t       pos_ix_bytes;       /* 1 o 2 si la posición va indexada, si no 0 */
    uint16_t      pos_ix_at;          /* dónde está ese índice en el vértice del FIFO */
} gx_vtx_plan_t;

/* Arma el plan de un formato (VCD de CP y el VAT `vat`). */
void gx_vtx_setup(const uint32_t vcd[2], const uint32_t vat[3], gx_vtx_plan_t *plan);

/* Lee un arreglo de la RAM: lo da quien llama (el PPU). NULL si no hay RAM ahí. */
typedef const uint8_t *(*gx_vtx_ram_fn)(void *ctx, uint32_t addr, uint32_t len);

/* Junta n vértices del FIFO (raw, n × raw_size bytes) en dst (hasta n ×
 * desc.stride). base y stride: los arreglos de CP. Devuelve cuántos quedaron
 * (los descartados no se escriben). Un arreglo fuera de la RAM da ceros. */
uint32_t gx_vtx_gather(const gx_vtx_plan_t *plan, const uint32_t base[16], const uint32_t stride[16],
                       gx_vtx_ram_fn ram, void *ram_ctx, const uint8_t *raw, uint32_t n,
                       uint8_t *dst, uint32_t *bad_reads);

/* Cuántos de los n vértices del FIFO no se descartan (sin leer la RAM). */
uint32_t gx_vtx_count_loaded(const gx_vtx_plan_t *plan, const uint8_t *raw, uint32_t n);

/* Decodifica n vértices juntados (n <= GX_VTX_BLOCK). Listo para SPU. */
void gx_vtx_decode(const gx_vtx_desc_t *d, const gx_vtx_defaults_t *def, const uint8_t *src,
                   uint32_t n, gx_vtx_in_t *out);

#endif
