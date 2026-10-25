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
 * Copia al XFB (texenc.h): EncodeXFB de SWEfbInterface.cpp y CopyRegion.h de
 * Dolphin cf5cb7ef.
 *
 *   1. Cada píxel del rectángulo con el de arriba y el de abajo: el filtro
 *      vertical (coeficientes 0-1, 2-4 y 5-6, suma >> 6 hasta 255). Sin el
 *      recorte de arriba o de abajo, los vecinos salen de fuera del rectángulo
 *      (hasta los bordes del EFB).
 *   2. La gamma: powf de la PC en Dolphin; acá una tabla que sale de la glibc
 *      de la referencia (tools/gamma/tabla.c).
 *   3. RGB a YUV (BT.601 con los redondeos de Dolphin), y a 4:2:2 promediando
 *      U y V con sus vecinos (1/4, 1/2, 1/4).
 *   4. La escala vertical con el vecino más cercano (round de double).
 *
 * El paso 3 escribe en un búfer de Dolphin que queda entre copias (static) en
 * la posición x del EFB dentro de filas del ancho de la copia, y el paso 4 lee
 * desde la x 0: con el rectángulo corrido a la derecha se ve parte de una
 * copia anterior. Se emula igual con el búfer que pasa quien llama.
 */
#include "gx/texenc.h"
#include "gx/x86.h"

#define EFB_W 640
#define EFB_H 528

/* GammaCorrection: (u8) clamp(powf(i / 255.0f, 1.0f / gamma) × 255.0f) con gamma 1,0, 1,7, 2,2
 * y 2,2 (tools/gamma/tabla.c, glibc 2.39, igual con FMA y sin FMA) */
static const uint8_t gamma_lut[4][256] = {
    {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
        48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
        64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
        80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
        96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
        112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
        128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
        144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
        160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
        176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
        192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
        208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
        224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
        240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
    },
    {
        0, 9, 14, 18, 22, 25, 28, 30, 33, 35, 37, 40, 42, 44, 46, 48,
        50, 51, 53, 55, 57, 58, 60, 61, 63, 65, 66, 68, 69, 70, 72, 73,
        75, 76, 77, 79, 80, 81, 83, 84, 85, 87, 88, 89, 90, 91, 93, 94,
        95, 96, 97, 98, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 112,
        113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 127,
        128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 139, 140, 141, 142,
        143, 144, 145, 146, 147, 147, 148, 149, 150, 151, 152, 152, 153, 154, 155, 156,
        157, 157, 158, 159, 160, 161, 162, 162, 163, 164, 165, 166, 166, 167, 168, 169,
        170, 170, 171, 172, 173, 173, 174, 175, 176, 176, 177, 178, 179, 179, 180, 181,
        182, 182, 183, 184, 185, 185, 186, 187, 188, 188, 189, 190, 190, 191, 192, 193,
        193, 194, 195, 195, 196, 197, 198, 198, 199, 200, 200, 201, 202, 202, 203, 204,
        205, 205, 206, 207, 207, 208, 209, 209, 210, 211, 211, 212, 213, 213, 214, 215,
        215, 216, 217, 217, 218, 219, 219, 220, 221, 221, 222, 222, 223, 224, 224, 225,
        226, 226, 227, 228, 228, 229, 230, 230, 231, 231, 232, 233, 233, 234, 235, 235,
        236, 236, 237, 238, 238, 239, 239, 240, 241, 241, 242, 243, 243, 244, 244, 245,
        246, 246, 247, 247, 248, 249, 249, 250, 250, 251, 252, 252, 253, 253, 254, 255,
    },
    {
        0, 20, 28, 33, 38, 42, 46, 49, 52, 55, 58, 61, 63, 65, 68, 70,
        72, 74, 76, 78, 80, 81, 83, 85, 87, 88, 90, 91, 93, 94, 96, 97,
        99, 100, 102, 103, 104, 106, 107, 108, 109, 111, 112, 113, 114, 115, 117, 118,
        119, 120, 121, 122, 123, 124, 125, 126, 128, 129, 130, 131, 132, 133, 134, 135,
        136, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 147, 148, 149,
        150, 151, 152, 153, 153, 154, 155, 156, 157, 158, 158, 159, 160, 161, 162, 162,
        163, 164, 165, 165, 166, 167, 168, 168, 169, 170, 171, 171, 172, 173, 174, 174,
        175, 176, 176, 177, 178, 178, 179, 180, 181, 181, 182, 183, 183, 184, 185, 185,
        186, 187, 187, 188, 189, 189, 190, 190, 191, 192, 192, 193, 194, 194, 195, 196,
        196, 197, 197, 198, 199, 199, 200, 200, 201, 202, 202, 203, 203, 204, 205, 205,
        206, 206, 207, 208, 208, 209, 209, 210, 210, 211, 212, 212, 213, 213, 214, 214,
        215, 216, 216, 217, 217, 218, 218, 219, 219, 220, 220, 221, 222, 222, 223, 223,
        224, 224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231, 231,
        232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 239, 239,
        240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247,
        248, 248, 249, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255,
    },
    {
        0, 20, 28, 33, 38, 42, 46, 49, 52, 55, 58, 61, 63, 65, 68, 70,
        72, 74, 76, 78, 80, 81, 83, 85, 87, 88, 90, 91, 93, 94, 96, 97,
        99, 100, 102, 103, 104, 106, 107, 108, 109, 111, 112, 113, 114, 115, 117, 118,
        119, 120, 121, 122, 123, 124, 125, 126, 128, 129, 130, 131, 132, 133, 134, 135,
        136, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 147, 148, 149,
        150, 151, 152, 153, 153, 154, 155, 156, 157, 158, 158, 159, 160, 161, 162, 162,
        163, 164, 165, 165, 166, 167, 168, 168, 169, 170, 171, 171, 172, 173, 174, 174,
        175, 176, 176, 177, 178, 178, 179, 180, 181, 181, 182, 183, 183, 184, 185, 185,
        186, 187, 187, 188, 189, 189, 190, 190, 191, 192, 192, 193, 194, 194, 195, 196,
        196, 197, 197, 198, 199, 199, 200, 200, 201, 202, 202, 203, 203, 204, 205, 205,
        206, 206, 207, 208, 208, 209, 209, 210, 210, 211, 212, 212, 213, 213, 214, 214,
        215, 216, 216, 217, 217, 218, 218, 219, 219, 220, 220, 221, 222, 222, 223, 223,
        224, 224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231, 231,
        232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 239, 239,
        240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247,
        248, 248, 249, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255,
    },
};

typedef struct yuv {
    uint8_t y;
    int8_t u, v;
} yuv_t;

static inline uint8_t c6to8(uint32_t v)
{
    return (uint8_t)((v << 2) | (v >> 4));
}

/* GetPixelColor: 0xRRGGBBAA */
static uint32_t get_color(const uint32_t *color, uint32_t pf, uint32_t x, uint32_t y)
{
    const uint32_t s = color[x + y * EFB_W] & 0xFFFFFFu;
    switch (pf) {
    case 0: case 2: case 3:
        return 0xFFu | s << 8;
    case 1:
        return c6to8(s & 0x3Fu) | (uint32_t)c6to8((s >> 6) & 0x3Fu) << 8 |
               (uint32_t)c6to8((s >> 12) & 0x3Fu) << 16 | (uint32_t)c6to8((s >> 18) & 0x3Fu) << 24;
    default:
        return 0;
    }
}

/* Corrimiento aritmético (en C, el de un negativo depende del compilador) */
static inline int32_t sar(int32_t v, unsigned n)
{
    return v < 0 ? -(int32_t)((uint32_t)(-(v + 1)) >> n) - 1 : (int32_t)((uint32_t)v >> n);
}

/* ConvertColorToYUV: (y >> 8) + bit 7, también para u y v con signo */
static yuv_t to_yuv(uint32_t r, uint32_t g, uint32_t b)
{
    const uint32_t y = (66 * r + 129 * g + 25 * b) & 0xFFFFu;
    const int32_t u = (int16_t)(-38 * (int32_t)r - 74 * (int32_t)g + 112 * (int32_t)b);
    const int32_t v = (int16_t)(112 * (int32_t)r - 94 * (int32_t)g - 18 * (int32_t)b);
    yuv_t o;
    o.y = (uint8_t)((y >> 8) + ((y >> 7) & 1u));
    o.u = (int8_t)(uint8_t)(sar(u, 8) + (sar(u, 7) & 1));
    o.v = (int8_t)(uint8_t)(sar(v, 8) + (sar(v, 7) & 1));
    return o;
}

/* El alto escalado: (int)(alto × escala) en float */
static int32_t scaled_height(const gx_xfbenc_t *c)
{
    return gx_x86_f2i((float)(int32_t)(c->bottom - c->top) * c->y_scale);
}

uint32_t gx_xfbenc_size(const gx_xfbenc_t *c)
{
    const int32_t h = scaled_height(c);
    return h > 0 ? (c->right - c->left) * (uint32_t)h * 2 : 0;
}

void gx_xfbenc(uint8_t *dst, uint8_t *source, const uint32_t *color, const gx_xfbenc_t *c)
{
    const uint32_t left = c->left, right = c->right, top = c->top, bottom = c->bottom;
    const uint32_t width = right - left;
    const uint8_t *lut = gamma_lut[c->gamma & 3u];
    const uint32_t k_prev = (uint32_t)c->coef[0] + c->coef[1];
    const uint32_t k_cur = (uint32_t)c->coef[2] + c->coef[3] + c->coef[4];
    const uint32_t k_next = (uint32_t)c->coef[5] + c->coef[6];

    /* En Dolphin sin inicializar: solo se lee lo que no se escribió con un
     * ancho impar corrido a la derecha (la x impar es un aviso) */
    yuv_t line[EFB_W + 2] = { { 0, 0, 0 } };
    uint32_t row = 0;                           /* en pares Y/UV */
    for (uint32_t y = top; y < bottom; y++) {
        const uint32_t lo = c->clamp_top ? top : 0;
        const uint32_t y_prev = (y >= 1 && y - 1 > lo) ? y - 1 : lo;
        const uint32_t hi = (c->clamp_bottom ? bottom : EFB_H) - 1;
        const uint32_t y_next = y + 1 < hi ? y + 1 : hi;

        for (uint32_t i = 1, x = left; x < right; i++, x++) {
            const uint32_t c0 = get_color(color, c->pf, x, y_prev);
            const uint32_t c1 = get_color(color, c->pf, x, y);
            const uint32_t c2 = get_color(color, c->pf, x, y_next);
            uint32_t rgb[3];
            for (int k = 0; k < 3; k++) {       /* b, g, r */
                const unsigned sh = 8u * (unsigned)(k + 1);
                const uint32_t sum = ((c0 >> sh) & 0xFFu) * k_prev + ((c1 >> sh) & 0xFFu) * k_cur +
                                     ((c2 >> sh) & 0xFFu) * k_next;
                rgb[k] = lut[(sum >> 6) < 255 ? sum >> 6 : 255];
            }
            line[i] = to_yuv(rgb[2], rgb[1], rgb[0]);
        }
        line[0] = line[1];
        line[right + 1] = line[right];

        for (uint32_t i = 1, x = left; x < right; i += 2, x += 2) {
            uint8_t *p = &source[(row + x) * 2];
            p[0] = (uint8_t)(line[i].y + 16);
            p[1] = (uint8_t)(128 + sar(line[i - 1].u + 2 * line[i].u + line[i + 1].u, 2));
            p[2] = (uint8_t)(line[i + 1].y + 16);
            p[3] = (uint8_t)(128 + sar(line[i - 1].v + 2 * line[i].v + line[i + 1].v, 2));
        }
        row += width;
    }

    /* CopyRegion al alto escalado: la fila round(y × alto / alto escalado) */
    const int32_t dh = scaled_height(c);
    if (dh <= 0)
        return;
    const double ratio = (double)(bottom - top) / (double)dh;
    for (uint32_t yo = 0; yo < (uint32_t)dh; yo++) {
        const double fy = (double)yo * ratio;
        uint32_t sy = (uint32_t)fy;
        if (fy - (double)sy >= 0.5)
            sy++;
        for (uint32_t xo = 0; xo < width; xo++) {
            /* Con escala de 2 o más y el EFB entero, Dolphin lee después del
             * final de su búfer: acá da 0 */
            const uint32_t si = sy * width + xo;
            dst[(yo * width + xo) * 2] = si < EFB_W * EFB_H ? source[si * 2] : 0;
            dst[(yo * width + xo) * 2 + 1] = si < EFB_W * EFB_H ? source[si * 2 + 1] : 0;
        }
    }
}
