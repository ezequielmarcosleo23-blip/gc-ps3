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
 * El XF (Fase 2, etapa B): posición, normales, color y alfa con las 8 luces y
 * las coordenadas de textura de cada vértice. Es TransformUnit.cpp del
 * renderizador por software de Dolphin (commit cf5cb7ef), con las mismas
 * operaciones de float en el mismo orden, para dar los mismos bits (D-043).
 *
 * Listo para SPU (D-041): recibe una foto del estado (gx_xf_snap_t, unos 7 KB)
 * y bloques de vértices decodificados; no lee nada más. El viewport (la
 * división por w y la escala a la pantalla) va después del recorte, en la
 * etapa C.
 */
#ifndef GCPS3_GX_XFORM_H
#define GCPS3_GX_XFORM_H

#include <stdint.h>

#include "gx/vtx.h"

#define GX_XF_MEM_WORDS 0x680u          /* matrices y luces: XF 0x000-0x67F */
#define GX_XF_REG_WORDS 0x58u           /* registros: XF 0x1000-0x1057 */

/* La foto del estado que usa el XF */
typedef struct gx_xf_snap {
    float    mem[GX_XF_MEM_WORDS];      /* matrices y luces, con los bits de cada palabra */
    uint32_t reg[GX_XF_REG_WORDS];
    float    tex_scale[8][2];           /* BP SU_SSIZE y SU_TSIZE: escala + 1 */
} gx_xf_snap_t;

/* Un vértice transformado (OutputVertexData antes del recorte) */
typedef struct gx_vtx_out {
    float   mv[3];                      /* en la vista */
    float   proj[4];                    /* proyectado (x, y, z, w) */
    float   nrm[3][3];                  /* normal (normalizada), tangente, binormal */
    float   tex[8][3];                  /* s, t, q */
    uint8_t col[2][4];                  /* r g b a, con las luces */
} gx_vtx_out_t;

/* Transforma n vértices (n <= GX_VTX_BLOCK). Listo para SPU. */
void gx_xform(const gx_xf_snap_t *xf, const gx_vtx_in_t *in, uint32_t n, gx_vtx_out_t *out);

/* Cuántas coordenadas genera el XF (XF 0x103F; más de 8 no existen). */
uint32_t gx_xf_num_texgens(const gx_xf_snap_t *xf);

/* float a int como la conversión de x86 (cvttss2si): fuera de rango o NaN da
 * INT32_MIN. Dolphin la usa al recortar los colores de las luces; en el PPU la
 * conversión de C satura distinto. */
int32_t gx_xf_to_int(float f);

#endif
