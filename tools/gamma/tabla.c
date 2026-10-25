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
 * La gamma de la copia al XFB como la calcula Dolphin (GammaCorrection de
 * VideoBackends/Software/SWEfbInterface.cpp): powf de la libm de la PC. Son
 * 4 × 256 valores (BP 0x52, bits 7-8: 1,0, 1,7, 2,2 y otra vez 2,2), así que
 * la tabla de src/gx/xfbenc.c sale de acá, con la glibc de la referencia.
 *
 *   gcc -std=c11 -O2 -ffp-contract=off tools/gamma/tabla.c -lm -o build/gamma-tabla
 *   build/gamma-tabla                      # la tabla en C
 *   GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2,-FMA build/gamma-tabla   # sin FMA: igual
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Por puntero: que el compilador no la calcule ni la cambie */
static float (*volatile powfp)(float, float) = powf;

int main(void)
{
    static const float gammas[4] = { 1.0f, 1.7f, 2.2f, 2.2f };
    printf("static const uint8_t gamma_lut[4][256] = {\n");
    for (int g = 0; g < 4; g++) {
        const float rcp = 1.0f / gammas[g];
        printf("    {");
        for (int i = 0; i < 256; i++) {
            float v = powfp((float)(uint8_t)i / 255.0f, rcp) * 255.0f;
            v = v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v;
            printf("%s%u,", i % 16 ? " " : "\n        ", (unsigned)(uint8_t)v);
        }
        printf("\n    },\n");
    }
    printf("};\n");
    return 0;
}
