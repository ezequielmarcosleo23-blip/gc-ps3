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
 * La imagen que sale por la VI: el XFB (YUV 4:2:2 en la RAM) pasado a RGB.
 *
 * Un cuadro se arma con el último campo impar y el último par que terminó la
 * VI, línea por medio, empezando por el de PRB menor (como Dolphin decide qué
 * campo tiene la primera línea). Si solo hay un campo con XFB, sus líneas se
 * repiten. Cada par de píxeles comparte U y V, sin interpolar (igual que el
 * decodificador de XFB de Dolphin).
 *
 * YUV a RGB con BT.601 en rango de estudio y solo enteros, para que la PC y
 * la PS3 den los mismos bytes:
 *   C = Y - 16, D = U - 128, E = V - 128
 *   R = (298 C + 409 E + 128) / 256
 *   G = (298 C - 100 D - 208 E + 128) / 256
 *   B = (298 C + 516 D + 128) / 256      (recortados a 0-255)
 */
#ifndef GCPS3_CORE_XFB_H
#define GCPS3_CORE_XFB_H

#include <stddef.h>
#include <stdint.h>

#include "core/mem.h"
#include "hw/vi.h"

/* Un píxel YUV a RGB (3 bytes). */
void gc_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t rgb[3]);

/* Una línea de XFB (width píxeles, pares Y0 U Y1 V) a RGB. */
void gc_xfb_line_to_rgb(const uint8_t *yuyv, uint32_t width, uint8_t *rgb);

/* Tamaño del cuadro que se puede armar con los campos de la VI; 0 si no hay
 * ninguno con XFB. Bytes = ancho x alto x 3. */
size_t gc_xfb_frame_size(const gc_vi_t *vi, uint32_t *width, uint32_t *height);

/* Arma el cuadro RGB en rgb (cap bytes). 0, o -1 si no hay cuadro o no entra.
 * Las líneas que caen fuera de la RAM quedan en negro. */
int gc_xfb_frame(gc_mem_t *m, const gc_vi_t *vi, uint8_t *rgb, size_t cap);

#endif
