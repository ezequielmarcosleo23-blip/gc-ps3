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
 * Fase 2, paso C6: las copias del EFB a textura (src/gx/texenc.c) y al XFB
 * (xfbenc.c), y el paso por el búfer intermedio a la RAM (copy.c). Casos
 * chicos con el valor calculado a mano; lo general lo compara el DOL gx_copy
 * contra Dolphin.
 */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/texenc.h"
#include "test.h"

enum { R4 = 0x0, RA4 = 0x2, RA8 = 0x3, RGB565 = 0x4, RGB5A3 = 0x5, RGBA8 = 0x6, A8 = 0x7, R8 = 0x8, G8 = 0x9,
       B8 = 0xA, RG8 = 0xB, GB8 = 0xC };

static gx_efb_t efb;
static uint8_t out[1024];

static void enc(uint32_t pf, uint32_t fmt, int intensity, int half, uint32_t first)
{
    gx_texenc_t c = { pf, fmt, intensity, half, 7, 3, 32 };
    const gx_pixels_t src = { efb.px + first, 2 * GX_EFB_PIXELS - first };
    memset(out, 0, sizeof out);
    gx_texenc(out, src, &c);
}

/* Desde RGB8: intensidad, los bytes en cada formato, RGBA8 en dos mitades */
static void test_copy_rgb8(void)
{
    memset(&efb, 0, sizeof efb);
    efb.px[0] = 0x123456u;
    efb.px[1] = 0xABCDEFu;
    enc(0, R8, 1, 0, 0);
    CHECK_EQ_U64(out[0], 0x37u);                    /* (4096 + 66 r + 129 g + 25 b) >> 8 */
    CHECK_EQ_U64(out[1], 0xBAu);
    enc(0, R8, 0, 0, 0);
    CHECK_EQ_U64(out[0], 0x12u);
    enc(0, R4, 0, 0, 0);
    CHECK_EQ_U64(out[0], 0x1Au);
    enc(0, RA4, 0, 0, 0);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0xF1FAu);
    enc(0, RA4, 1, 0, 0);
    CHECK_EQ_U64(out[0], 0xF3u);
    enc(0, RA8, 0, 0, 0);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0xFF12u);
    enc(0, RGB565, 0, 0, 0);
    CHECK_EQ_U64(out[2] << 8 | out[3], 0xAE7Du);
    enc(0, RGB5A3, 0, 0, 0);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0x88CAu);
    enc(0, RGBA8, 0, 0, 0);
    CHECK_EQ_U64((uint64_t)out[0] << 24 | out[1] << 16 | out[32] << 8 | out[33], 0xFF123456u);
    CHECK_EQ_U64((uint64_t)out[2] << 24 | out[3] << 16 | out[34] << 8 | out[35], 0xFFABCDEFu);
    enc(0, A8, 0, 0, 0);
    CHECK_EQ_U64(out[0], 0xFFu);
    enc(2, RG8, 0, 0, 0);                            /* RGB565 se guarda como RGB8 */
    CHECK_EQ_U64(out[0] << 8 | out[1], 0x3412u);
    enc(0, GB8, 0, 0, 0);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0x5634u);
}

/* Desde RGBA6: de 6 a 8 bits, R4 y RGB5A3 desde los bits */
static void test_copy_rgba6(void)
{
    memset(&efb, 0, sizeof efb);
    efb.px[0] = 0x3Fu << 18 | 0x20u << 12 | 0x10u << 6 | 0x31u;
    efb.px[1] = 0x15u << 18 | 0x3Fu;
    enc(1, RA8, 0, 0, 0);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0xC7FFu);
    enc(1, R4, 0, 0, 0);
    CHECK_EQ_U64(out[0], 0xF5u);
    enc(1, RGB5A3, 0, 0, 0);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0x6F84u);    /* alfa < 7: 4443 */
    CHECK_EQ_U64(out[2] << 8 | out[3], 0xA800u);    /* alfa 7: 555 */
    enc(1, G8, 0, 0, 0);
    CHECK_EQ_U64(out[0], 0x82u);
}

/* Media escala: el promedio de 2 × 2 en RGB8, s + s >> 6 en RGBA6 */
static void test_copy_half(void)
{
    memset(&efb, 0, sizeof efb);
    efb.px[0] = 10u << 16;
    efb.px[1] = 20u << 16;
    efb.px[640] = 30u << 16;
    efb.px[641] = 41u << 16;
    enc(0, R8, 0, 1, 0);
    CHECK_EQ_U64(out[0], 25u);
    for (int i = 0; i < 4; i++)
        efb.px[(i >> 1) * 640 + (i & 1)] = (uint32_t)(1 + i) << 18 | 0x3Fu;
    enc(1, R8, 0, 1, 0);
    CHECK_EQ_U64(out[0], 10u);
    enc(1, A8, 0, 1, 0);
    CHECK_EQ_U64(out[0], 255u);
}

/* La Z: los bytes alto, medio y bajo, y lo cambiado de Dolphin a media escala */
static void test_copy_z(void)
{
    memset(&efb, 0, sizeof efb);
    for (int i = 0; i < 4; i++)
        efb.px[GX_EFB_PIXELS + (uint32_t)(i >> 1) * 640 + (uint32_t)(i & 1)] = 0xABCDEFu;
    enc(3, R8, 1, 0, GX_EFB_PIXELS);                 /* sin intensidad en la Z */
    CHECK_EQ_U64(out[0], 0xABu);
    enc(3, RG8, 0, 0, GX_EFB_PIXELS);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0xCDABu);
    enc(3, GB8, 0, 0, GX_EFB_PIXELS);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0xEFCDu);
    enc(3, RGBA8, 0, 1, GX_EFB_PIXELS);
    CHECK_EQ_U64((uint64_t)out[0] << 24 | out[1] << 16 | out[32] << 8 | out[33], 0xFFEFCDABu);
    enc(3, RG8, 0, 1, GX_EFB_PIXELS);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0xCDEFu);
    enc(3, GB8, 0, 1, GX_EFB_PIXELS);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0xABCDu);
    const gx_texenc_t bad = { 3, RA8, 0, 0, 7, 3, 32 };
    CHECK_EQ_U64(gx_texenc_extent(&bad), 0);        /* en Dolphin, PanicAlert */
}

/* El relleno del bloque: a la derecha sigue la fila; abajo del color, la Z */
static void test_copy_padding(void)
{
    memset(&efb, 0, sizeof efb);
    efb.px[6] = 0x66u << 16;
    efb.px[7] = 0x77u << 16;
    gx_texenc_t c = { 0, R8, 0, 0, 5, 1, 32 };       /* 6 × 2 */
    gx_pixels_t src = { efb.px, 2 * GX_EFB_PIXELS };
    memset(out, 0, sizeof out);
    gx_texenc(out, src, &c);
    CHECK_EQ_U64(out[6] << 8 | out[7], 0x6677u);
    CHECK_EQ_U64(gx_texenc_extent(&c), 32);
    efb.px[GX_EFB_PIXELS] = 0x5Au << 16;             /* la Z de (0, 0) */
    const uint32_t first = 526 * GX_EFB_WIDTH;
    src.p = efb.px + first;
    src.n = 2 * GX_EFB_PIXELS - first;
    memset(out, 0, sizeof out);
    gx_texenc(out, src, &c);
    CHECK_EQ_U64(out[16], 0x5Au);                    /* fila 2 del bloque */
}

/* Con un paso menor que la fila, a la RAM va solo lo que entra en el paso */
static void test_copy_ram_stride(void)
{
    static gc_system_t sys;
    gc_log_set_sink(NULL);
    CHECK(gc_system_init(&sys) == 0);
    for (uint32_t x = 0; x < 16; x++)
        sys.gx.efb->px[x] = (0x10u + x) << 16;
    gc_gx_load_bp(&sys.gx, 0x49, 0);
    gc_gx_load_bp(&sys.gx, 0x4A, 15 | 3u << 10);     /* 16 × 4: dos bloques de I8 */
    gc_gx_load_bp(&sys.gx, 0x4B, 0x20000 >> 5);
    gc_gx_load_bp(&sys.gx, 0x4D, 1);                 /* paso 32 */
    gc_gx_load_bp(&sys.gx, 0x52, 1u << 3);           /* R8 */
    const uint8_t *ram = sys.mem.ram + 0x20000;
    CHECK_EQ_U64(ram[0] << 8 | ram[7], 0x1017u);
    CHECK_EQ_U64(ram[32], 0);
    gc_gx_load_bp(&sys.gx, 0x4D, 2);                 /* paso 64: la fila entera */
    gc_gx_load_bp(&sys.gx, 0x52, 1u << 3);
    CHECK_EQ_U64(ram[32] << 8 | ram[39], 0x181Fu);
    gc_system_free(&sys);
}

static void xfb(gx_xfbenc_t *c, uint32_t right, uint32_t bottom)
{
    static const uint8_t def[7] = { 0, 0, 21, 22, 21, 0, 0 };
    memset(c, 0, sizeof *c);
    c->right = right;
    c->bottom = bottom;
    c->clamp_top = c->clamp_bottom = 1;
    memcpy(c->coef, def, sizeof def);
    c->y_scale = 1.0f;
}

/* Al XFB: YUV de blanco y de rojo, el filtro, la gamma y la escala */
static void test_copy_xfb(void)
{
    static uint8_t source[GX_EFB_PIXELS * 2];
    memset(&efb, 0, sizeof efb);
    memset(source, 0, sizeof source);
    gx_xfbenc_t c;
    for (int x = 0; x < 4; x++) {
        efb.px[x] = 0xFF0000u;
        efb.px[640 + x] = 0xFFFFFFu;
        efb.px[1280 + x] = 0x010101u;
    }
    xfb(&c, 4, 2);
    CHECK_EQ_U64(gx_xfbenc_size(&c), 16);
    gx_xfbenc(out, source, efb.px, &c);
    CHECK_EQ_U64((uint64_t)out[0] << 24 | out[1] << 16 | out[2] << 8 | out[3], 0x525A52F0u);
    CHECK_EQ_U64((uint64_t)out[8] << 24 | out[9] << 16 | out[10] << 8 | out[11], 0xEB80EB80u);

    /* Todo el peso arriba: la fila 1 sale roja; con el recorte de arriba en
     * la fila 1, blanca */
    c.coef[0] = 64;
    c.coef[2] = c.coef[3] = c.coef[4] = 0;
    gx_xfbenc(out, source, efb.px, &c);
    CHECK_EQ_U64(out[8] << 8 | out[9], 0x525Au);
    c.top = 1;
    gx_xfbenc(out, source, efb.px, &c);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0xEB80u);

    /* Gamma 1,7: 1 pasa a 9 */
    xfb(&c, 4, 3);
    c.top = 2;
    c.gamma = 1;
    gx_xfbenc(out, source, efb.px, &c);
    CHECK_EQ_U64(out[0] << 8 | out[1], 0x1880u);    /* y = (220 × 9) >> 8 + 1 + 16 */

    /* Escala 2: filas 0, 1, 1, 2 de 2: la 2 es la de una copia anterior */
    xfb(&c, 4, 2);
    c.y_scale = 2.0f;
    memset(source + 2 * 4 * 2, 0x11, 8);
    CHECK_EQ_U64(gx_xfbenc_size(&c), 32);
    gx_xfbenc(out, source, efb.px, &c);
    CHECK_EQ_U64(out[0] << 16 | out[8] << 8 | out[16], 0x52EBEBu);
    CHECK_EQ_U64(out[24], 0x11u);
}

const test_case_t copy_tests[] = {
    { "copia: de RGB8 a cada formato de textura", test_copy_rgb8 },
    { "copia: de RGBA6, con los bits de 6", test_copy_rgba6 },
    { "copia: media escala", test_copy_half },
    { "copia: de la Z", test_copy_z },
    { "copia: el relleno de los bloques sigue de corrido", test_copy_padding },
    { "copia: a la RAM con un paso menor que la fila", test_copy_ram_stride },
    { "copia: al XFB con filtro, gamma y escala", test_copy_xfb },
    { NULL, NULL },
};
