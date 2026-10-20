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
 * DOL de prueba "pantalla": "hola mundo" en el XFB y la VI en 480i NTSC.
 * Dibuja, prende la VI, espera un cuadro entero e imprime el CRC del XFB;
 * después se queda mostrando la imagen.
 *
 * En la PC (GCPS3_HOST) dibuja lo mismo en memoria: la salida es
 * tests/dol/esperado/pantalla.txt. Con un nombre de archivo como argumento,
 * además guarda la imagen en PPM para mirarla.
 */
#include "rt.h"
#include "video.h"

static uint32_t draw(void)
{
    static const char big[] = "hola mundo", small[] = "gc-ps3 fase 1: pantalla";
    video_fill(COLOR_FONDO);
    video_rect(0, 40, XFB_PAIRS, 4, COLOR_CELESTE);
    video_text((XFB_PAIRS - video_text_pairs(big, 3)) / 2, 132, 3, COLOR_BLANCO, big);
    video_text((XFB_PAIRS - video_text_pairs(small, 1)) / 2, 220, 1, COLOR_AMARILLO, small);
    video_rect(0, 316, XFB_PAIRS, 4, COLOR_CELESTE);
    video_bars(360, 80);
    video_flush();
    return video_crc32();
}

static void run(void)
{
    const uint32_t crc = draw();
    video_init_480i();
    OSReport("pantalla: VI en 480i NTSC, %ux%u, XFB en 0x%08x\n", XFB_WIDTH, XFB_HEIGHT, XFB_PHYS);
    video_wait_frame();                          /* el cuadro en curso puede ser viejo */
    video_wait_frame();                          /* este ya se vio entero */
    OSReport("pantalla: crc32 del XFB 0x%08x\n", crc);
    OSReport("pantalla: listo, la imagen queda en pantalla\n");
}

#ifdef GCPS3_HOST
/* YUV 4:2:2 a RGB (BT.601) solo para mirar el dibujo en la PC */
static unsigned char clamp(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", XFB_WIDTH, XFB_HEIGHT);
    const uint32_t *p = video_xfb();
    for (uint32_t i = 0; i < XFB_BYTES / 4; i++) {
        const int y0 = (int)(p[i] >> 24) - 16, u = (int)(p[i] >> 16 & 0xFF) - 128;
        const int y1 = (int)(p[i] >> 8 & 0xFF) - 16, v = (int)(p[i] & 0xFF) - 128;
        for (int k = 0; k < 2; k++) {
            const int y = 298 * (k ? y1 : y0);
            const unsigned char rgb[3] = { clamp((y + 409 * v + 128) >> 8),
                                           clamp((y - 100 * u - 208 * v + 128) >> 8),
                                           clamp((y + 516 * u + 128) >> 8) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    run();
    if (argc > 1)
        save_ppm(argv[1]);
    return 0;
}
#else
int main(void)
{
    run();
    for (;;)
        video_wait_frame();
}
#endif
