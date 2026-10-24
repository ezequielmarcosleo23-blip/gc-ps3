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
 * Fase 2, etapa C4: los decodificadores de texturas (src/gx/texdec.c), la TMEM
 * con sus cargas y el muestreador (src/gx/tex.c). Valores a mano; lo general
 * lo compara el DOL gx_tex contra Dolphin.
 */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/tev.h"
#include "gx/texdec.h"
#include "test.h"

static gc_system_t sys;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static int setup(void)
{
    gc_log_set_sink(quiet);
    return gc_system_init(&sys);
}

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

static uint32_t rgba(const uint8_t t[4])
{
    return (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
}

static uint32_t texel(const uint8_t *data, uint32_t n, int32_t s, int32_t t, int32_t w_m1, uint32_t fmt,
                      const uint8_t *tlut, uint32_t tlutfmt)
{
    uint8_t out[4] = { 0xEE, 0xEE, 0xEE, 0xEE };
    const gx_span_t src = { data, n }, pal = { tlut, tlut ? 32768u : 0u };
    gx_tex_decode_texel(out, src, s, t, w_m1, fmt, pal, tlutfmt);
    return rgba(out);
}

/* Un texel de cada formato directo, en su posición del bloque */
static void test_tex_formats(void)
{
    static uint8_t d[128];
    memset(d, 0, sizeof d);
    /* I4: bloque de 8 × 8, dos texels por byte, el par arriba */
    d[(1 * 8 + 3) / 2] = 0x5A;                          /* (2,1) = 5, (3,1) = A */
    CHECK_EQ_U64(texel(d, 128, 2, 1, 7, GX_TF_I4, NULL, 0), 0x55555555u);
    CHECK_EQ_U64(texel(d, 128, 3, 1, 7, GX_TF_I4, NULL, 0), 0xAAAAAAAAu);
    /* IA4: alfa arriba, intensidad abajo */
    memset(d, 0, sizeof d);
    d[2 * 8 + 5] = 0x3C;
    CHECK_EQ_U64(texel(d, 128, 5, 2, 7, GX_TF_IA4, NULL, 0), 0xCCCCCC33u);
    /* IA8: alfa en el primer byte; bloques de 4 × 4, el segundo bloque en x */
    memset(d, 0, sizeof d);
    d[32 + 2 * ((1 * 4 + 2))] = 0x80;
    d[32 + 2 * ((1 * 4 + 2)) + 1] = 0x40;
    CHECK_EQ_U64(texel(d, 128, 6, 1, 7, GX_TF_IA8, NULL, 0), 0x40404080u);
    /* RGB565 y RGB5A3 (los dos modos), big-endian */
    memset(d, 0, sizeof d);
    d[0] = 0xF8; d[1] = 0x1F;                           /* r 31, g 0, b 31 */
    d[2] = 0xFC; d[3] = 0x00;                           /* RGB5A3 opaco: r 31, g 0, b 0 */
    d[4] = 0x5A; d[5] = 0x3C;                           /* RGB5A3: a 5, r A, g 3, b C */
    CHECK_EQ_U64(texel(d, 128, 0, 0, 3, GX_TF_RGB565, NULL, 0), 0xFF00FFFFu);
    CHECK_EQ_U64(texel(d, 128, 1, 0, 3, GX_TF_RGB5A3, NULL, 0), 0xFF0000FFu);
    CHECK_EQ_U64(texel(d, 128, 2, 0, 3, GX_TF_RGB5A3, NULL, 0), 0xAA33CCB6u);
    /* RGBA8: alfa y rojo en los primeros 32 bytes del bloque, verde y azul después */
    memset(d, 0, sizeof d);
    d[2 * 5] = 0x11; d[2 * 5 + 1] = 0x22; d[32 + 2 * 5] = 0x33; d[32 + 2 * 5 + 1] = 0x44;
    CHECK_EQ_U64(texel(d, 128, 1, 1, 3, GX_TF_RGBA8, NULL, 0), 0x22334411u);
    /* Fuera del búfer: 0 */
    CHECK_EQ_U64(texel(d, 40, 1, 1, 3, GX_TF_RGBA8, NULL, 0), 0x22000011u);
    /* Un formato que no existe no escribe */
    CHECK_EQ_U64(texel(d, 128, 0, 0, 3, 7, NULL, 0), 0xEEEEEEEEu);
}

/* C4, C8 y C14X2 con las tres paletas; CMPR con c1 > c2 y c1 <= c2 */
static void test_tex_palette_cmpr(void)
{
    static uint8_t d[64], tlut[32768];
    memset(d, 0, sizeof d);
    memset(tlut, 0, sizeof tlut);
    tlut[2 * 7] = 0x80; tlut[2 * 7 + 1] = 0x30;         /* IA8: a 0x80, i 0x30 */
    tlut[2 * 0x123] = 0x07; tlut[2 * 0x123 + 1] = 0xE0;  /* RGB565: verde 63 */
    d[0] = 0x70;                                        /* C4 (0,0) = 7 */
    CHECK_EQ_U64(texel(d, 64, 0, 0, 7, GX_TF_C4, tlut, GX_TL_IA8), 0x30303080u);
    d[0] = 0x07;
    CHECK_EQ_U64(texel(d, 64, 0, 0, 7, GX_TF_C8, tlut, GX_TL_IA8), 0x30303080u);
    d[0] = 0xC1; d[1] = 0x23;                           /* C14X2: los 2 bits de arriba no cuentan */
    CHECK_EQ_U64(texel(d, 64, 0, 0, 3, GX_TF_C14X2, tlut, GX_TL_RGB565), 0x00FF00FFu);
    tlut[2 * 0x123] = 0x0F; tlut[2 * 0x123 + 1] = 0x00;
    CHECK_EQ_U64(texel(d, 64, 0, 0, 3, GX_TF_C14X2, tlut, GX_TL_RGB5A3), 0xFF000000u);

    /* CMPR: c1 = rojo puro, c2 = azul puro (c1 > c2); fila 0: 0 1 2 3 */
    memset(d, 0, sizeof d);
    d[0] = 0xF8; d[1] = 0x00; d[2] = 0x00; d[3] = 0x1F; d[4] = 0x1B;
    CHECK_EQ_U64(texel(d, 64, 0, 0, 7, GX_TF_CMPR, NULL, 0), 0xFF0000FFu);
    CHECK_EQ_U64(texel(d, 64, 1, 0, 7, GX_TF_CMPR, NULL, 0), 0x0000FFFFu);
    CHECK_EQ_U64(texel(d, 64, 2, 0, 7, GX_TF_CMPR, NULL, 0), 0x9F005FFFu);   /* (255 × 5) >> 3 y (255 × 3) >> 3 */
    CHECK_EQ_U64(texel(d, 64, 3, 0, 7, GX_TF_CMPR, NULL, 0), 0x5F009FFFu);
    /* c1 <= c2: el 2 es el promedio y el 3, el promedio transparente */
    d[0] = 0x00; d[1] = 0x1F; d[2] = 0xF8; d[3] = 0x00;
    CHECK_EQ_U64(texel(d, 64, 2, 0, 7, GX_TF_CMPR, NULL, 0), 0x7F007FFFu);
    CHECK_EQ_U64(texel(d, 64, 3, 0, 7, GX_TF_CMPR, NULL, 0), 0x7F007F00u);
    /* El segundo sub-bloque en x */
    d[8] = 0xFF; d[9] = 0xFF; d[12] = 0x00;
    CHECK_EQ_U64(texel(d, 64, 4, 0, 7, GX_TF_CMPR, NULL, 0), 0xFFFFFFFFu);
}

/* La textura entera de a bloques da lo mismo que texel por texel */
static void test_tex_decode_image(void)
{
    static uint8_t data[4096], tlut[32768], img[24 * 20 * 4];
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof data; i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (uint8_t)(x >> 16);
    }
    for (size_t i = 0; i < sizeof tlut; i++)
        tlut[i] = (uint8_t)(i * 37u + (i >> 8));
    static const uint32_t fmts[11] = { GX_TF_I4, GX_TF_I8, GX_TF_IA4, GX_TF_IA8, GX_TF_RGB565, GX_TF_RGB5A3,
                                       GX_TF_RGBA8, GX_TF_C4, GX_TF_C8, GX_TF_C14X2, GX_TF_CMPR };
    static const uint32_t sizes[4][2] = { { 1, 1 }, { 13, 7 }, { 16, 16 }, { 24, 20 } };
    const gx_span_t src = { data, sizeof data }, pal = { tlut, sizeof tlut };
    uint32_t bad = 0;
    for (int f = 0; f < 11; f++)
        for (int z = 0; z < 4; z++)
            for (uint32_t tf = 0; tf < 3; tf++) {
                const uint32_t w = sizes[z][0], h = sizes[z][1];
                gx_tex_decode(img, src, w, h, fmts[f], pal, tf);
                for (uint32_t t = 0; t < h; t++)
                    for (uint32_t s = 0; s < w; s++) {
                        uint8_t one[4];
                        gx_tex_decode_texel(one, src, (int32_t)s, (int32_t)t, (int32_t)w - 1, fmts[f], pal, tf);
                        if (memcmp(one, img + 4 * (t * w + s), 4) != 0)
                            bad++;
                    }
            }
    CHECK_EQ_U64(bad, 0);
}

/* Registros de la unidad de textura 0 */
static void unit0(uint32_t mode0, uint32_t w, uint32_t h, uint32_t fmt, uint32_t addr)
{
    gc_gx_load_bp(&sys.gx, 0x80, mode0);
    gc_gx_load_bp(&sys.gx, 0x84, 0);
    gc_gx_load_bp(&sys.gx, 0x88, (w - 1) | (h - 1) << 10 | fmt << 20);
    gc_gx_load_bp(&sys.gx, 0x8C, 0);                    /* desde la RAM */
    gc_gx_load_bp(&sys.gx, 0x94, addr >> 5);
}

static uint32_t sample(int32_t s, int32_t t, int32_t lod, int linear)
{
    uint8_t out[4];
    gx_tex_sample(&sys.gx, s, t, lod, linear, 0, out);
    return rgba(out);
}

/* I8 de 4 × 4 con el valor 16 × t + s en un bloque de 8 × 4: envolturas y
 * filtro bilineal (medio texel corrido) */
static void test_tex_sampler(void)
{
    CHECK(setup() == 0);
    uint8_t *ram = sys.mem.ram + 0x10000;
    for (int t = 0; t < 4; t++)
        for (int s = 0; s < 8; s++)
            ram[t * 8 + s] = (uint8_t)(0x10 * t + s);
    unit0(0, 4, 4, GX_TF_I8, 0x10000);                  /* recortar */
    CHECK_EQ_U64(sample(2 * 128 + 5, 3 * 128, 0, 0), 0x32323232u);
    CHECK_EQ_U64(sample(-300, 9 * 128, 0, 0), 0x30303030u);
    unit0(1 | 1u << 2, 4, 4, GX_TF_I8, 0x10000);        /* repetir */
    CHECK_EQ_U64(sample(5 * 128, 6 * 128, 0, 0), 0x21212121u);
    unit0(2 | 2u << 2, 4, 4, GX_TF_I8, 0x10000);        /* espejo: 5 → 2, -1 → 0 */
    CHECK_EQ_U64(sample(5 * 128, -1 * 128, 0, 0), 0x02020202u);
    unit0(1, 3, 4, GX_TF_I8, 0x10000);                  /* repetir sin potencia de 2: AND 2 */
    CHECK_EQ_U64(sample(7 * 128, 0, 0, 0), 0x02020202u);
    CHECK_EQ_U64(sample(5 * 128, 0, 0, 0), 0);
    /* Bilineal en el borde de 4 texels: (1,1) (2,1) (1,2) (2,2) a partes iguales */
    unit0(0, 4, 4, GX_TF_I8, 0x10000);
    CHECK_EQ_U64(sample(2 * 128, 2 * 128, 0, 1), 0x19191919u);       /* (0x11+0x12+0x21+0x22)/4 = 0x19,8 */
    teardown();
}

/* Mipmaps: el nivel 1 empieza después del nivel 0; más cercano redondea desde
 * ,5 y lineal mezcla en dieciseisavos */
static void test_tex_mipmaps(void)
{
    CHECK(setup() == 0);
    uint8_t *ram = sys.mem.ram + 0x20000;
    memset(ram, 0x40, 8 * 8);                           /* nivel 0: I8 de 8 × 8 (2 bloques) */
    memset(ram + 64, 0xC0, 32);                         /* nivel 1: 4 × 4 en un bloque de 8 × 4 */
    unit0(1u << 5, 8, 8, GX_TF_I8, 0x20000);            /* más cercano */
    CHECK_EQ_U64(sample(0, 0, 0x17, 0), 0xC0C0C0C0u);   /* 1 + 7/16: nivel 1 */
    CHECK_EQ_U64(sample(0, 0, 0x07, 0), 0x40404040u);
    CHECK_EQ_U64(sample(0, 0, 0x08, 0), 0xC0C0C0C0u);   /* 8/16: redondea al 1 */
    unit0(2u << 5, 8, 8, GX_TF_I8, 0x20000);            /* lineal */
    CHECK_EQ_U64(sample(0, 0, 0x04, 0), 0x60606060u);   /* (0x40 × 12 + 0xC0 × 4) / 16 */
    unit0(0, 8, 8, GX_TF_I8, 0x20000);                  /* sin mipmaps */
    CHECK_EQ_U64(sample(0, 0, 0x18, 0), 0x40404040u);
    teardown();
}

/* TMEM: paleta por BP 0x64/0x65 y textura precargada (también RGBA8 partida) */
static void test_tex_tmem(void)
{
    CHECK(setup() == 0);
    uint8_t *ram = sys.mem.ram;
    memset(ram + 0x30000, 0, 64);
    ram[0x30000 + 2 * 5] = 0xFF;                        /* entrada 5: IA8 a 0xFF, i 0x77 */
    ram[0x30000 + 2 * 5 + 1] = 0x77;
    gc_gx_load_bp(&sys.gx, 0x64, 0x30000 >> 5);
    gc_gx_load_bp(&sys.gx, 0x65, 2 | 1u << 10);         /* línea 2 × 512 bytes, 1 línea de 32 */
    CHECK_EQ_U64(sys.gx.tmem[1024 + 11], 0x77);
    /* C4 desde la RAM con la paleta de la TMEM */
    ram[0x31000] = 0x50;
    unit0(0, 8, 8, GX_TF_C4, 0x31000);
    gc_gx_load_bp(&sys.gx, 0x98, 2 | GX_TL_IA8 << 10);
    CHECK_EQ_U64(sample(0, 0, 0, 0), 0x777777FFu);

    /* Precarga de I8 a la línea 100 y lectura desde la TMEM */
    memset(ram + 0x32000, 0x5C, 32);
    gc_gx_load_bp(&sys.gx, 0x60, 0x32000 >> 5);
    gc_gx_load_bp(&sys.gx, 0x61, 100);
    gc_gx_load_bp(&sys.gx, 0x63, 1);
    CHECK_EQ_U64(sys.gx.tmem[100 * 32 + 31], 0x5C);
    unit0(0, 8, 4, GX_TF_I8, 0);
    gc_gx_load_bp(&sys.gx, 0x8C, 100 | 1u << 21);
    CHECK_EQ_U64(sample(3 * 128, 2 * 128, 0, 0), 0x5C5C5C5Cu);

    /* RGBA8 precargado: alfa y rojo a la línea 200, verde y azul a la 300 */
    for (int i = 0; i < 32; i++) {
        ram[0x33000 + i] = (uint8_t)(i & 1 ? 0xA1 : 0x7F);    /* a 0x7F, r 0xA1 */
        ram[0x33020 + i] = (uint8_t)(i & 1 ? 0xC3 : 0xB2);    /* g 0xB2, b 0xC3 */
    }
    gc_gx_load_bp(&sys.gx, 0x60, 0x33000 >> 5);
    gc_gx_load_bp(&sys.gx, 0x61, 200);
    gc_gx_load_bp(&sys.gx, 0x62, 300);
    gc_gx_load_bp(&sys.gx, 0x63, 1 | 3u << 15);
    unit0(0, 4, 4, GX_TF_RGBA8, 0);
    gc_gx_load_bp(&sys.gx, 0x8C, 200 | 1u << 21);
    gc_gx_load_bp(&sys.gx, 0x90, 300);
    CHECK_EQ_U64(sample(128, 128, 0, 0), 0xA1B2C37Fu);
    /* Desde la RAM fuera del final: 0 */
    unit0(0, 4, 4, GX_TF_RGBA8, GC_RAM_SIZE - 16);
    CHECK_EQ_U64(sample(3 * 128, 3 * 128, 0, 0), 0);
    CHECK_EQ_U64(sys.mem.bad, 0);
    teardown();
}

const test_case_t tex_tests[] = {
    { "tex: formatos directos, fuera del búfer y formato inválido", test_tex_formats },
    { "tex: paletas y CMPR", test_tex_palette_cmpr },
    { "tex: la textura entera igual a texel por texel", test_tex_decode_image },
    { "tex: envolturas y filtro bilineal", test_tex_sampler },
    { "tex: mipmaps más cercano y lineal", test_tex_mipmaps },
    { "tex: paleta y precarga en la TMEM", test_tex_tmem },
    { NULL, NULL },
};
