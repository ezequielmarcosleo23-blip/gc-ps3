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
 * Video para los DOLs de prueba, sin SDK: programa la VI en 480i NTSC (los
 * mismos tiempos que usa el SDK, ver src/hw/vi.h) y dibuja en un XFB de
 * 640x480 en YUV 4:2:2 con una fuente propia de 8x8.
 *
 * Cada palabra de 32 bits del XFB son dos píxeles: Y0 U Y1 V. Para no partir
 * el color entre píxeles, se dibuja de a pares: las x van en pares (0-319) y
 * cada punto de la fuente con escala k ocupa k pares (2k píxeles) y 2k líneas.
 *
 * Con GCPS3_HOST el XFB es un arreglo de la PC y los registros no se tocan:
 * el mismo dibujo da el mismo CRC (tests/dol/esperado/pantalla.txt).
 */
#ifndef GCPS3_TESTDOL_VIDEO_H
#define GCPS3_TESTDOL_VIDEO_H

#include <stdint.h>

#define XFB_PHYS   0x00200000u              /* 2 MB: lejos del programa y de la pila */
#define XFB_WIDTH  640u
#define XFB_HEIGHT 480u
#define XFB_PAIRS  (XFB_WIDTH / 2)          /* palabras por línea */
#define XFB_BYTES  (XFB_WIDTH * XFB_HEIGHT * 2)

/* Colores en Y U Y V (BT.601, rango de estudio) */
#define COLOR_FONDO    0x2AA12A6Eu          /* azul oscuro, RGB (0, 32, 96) */
#define COLOR_BLANCO   0xEB80EB80u
#define COLOR_AMARILLO 0xC11BC19Fu          /* RGB (255, 220, 0) */
#define COLOR_CELESTE  0xAFA3AF5Du          /* RGB (128, 200, 255) */
#define COLOR_NEGRO    0x10801080u

uint32_t *video_xfb(void);

/* VI en 480i NTSC con el XFB (campo impar arriba, par una línea más abajo),
 * DI0 y DI1 habilitadas (una por campo) y la salida prendida. */
void video_init_480i(void);

/* Espera el comienzo del cuadro siguiente (mira la línea del haz, DPV). */
void video_wait_frame(void);

/* Escribe el XFB de la caché a la RAM para que lo vea la VI. */
void video_flush(void);

void video_fill(uint32_t color);
void video_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void video_text(uint32_t x, uint32_t y, uint32_t scale, uint32_t color, const char *s);

/* Ancho en pares de un texto con escala k */
uint32_t video_text_pairs(const char *s, uint32_t scale);

/* Barras de color al 75 % (blanco, amarillo, cian, verde, magenta, rojo, azul, negro) */
void video_bars(uint32_t y, uint32_t h);

/* CRC-32 (el de zlib) del XFB como bytes Y U Y V, en el orden de la memoria del Gekko */
uint32_t video_crc32(void);

#endif
