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
 * Fase 2, etapa C3: el rasterizador (src/gx/raster.c), el TEV (tev.c), la
 * mezcla y la Z (efb.c), la caja envolvente (bbox.c) y la lectura del EFB por
 * el CPU. Casos chicos con el valor calculado a mano; lo general lo compara
 * el DOL gx_raster contra Dolphin.
 */
#include <math.h>
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/exp2.h"
#include "gx/raster.h"
#include "test.h"

static gc_system_t sys;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void bp(uint8_t reg, uint32_t v)
{
    gc_gx_load_bp(&sys.gx, reg, v);
}

#define CC(a, b, c, d) ((uint32_t)(a) << 12 | (uint32_t)(b) << 8 | (uint32_t)(c) << 4 | (uint32_t)(d) | 1u << 19)
#define AC(a, b, c, d) ((uint32_t)(a) << 13 | (uint32_t)(b) << 10 | (uint32_t)(c) << 7 | (uint32_t)(d) << 4 | 1u << 19)

/* Una etapa que pasa el color, tijera en todo el EFB, sin prueba de alfa,
 * RGB8 sin Z y escritura de color y alfa */
static int setup(void)
{
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    bp(0x00, 1u << 4);
    bp(0x20, 342 | 342u << 12);
    bp(0x21, (342 + 527) | (342u + 639) << 12);
    bp(0x59, 171 | 171u << 10);
    bp(0xC0, CC(15, 15, 15, 10));
    bp(0xC1, AC(7, 7, 7, 5));
    bp(0xF6, 1u << 2);
    bp(0xF7, 2u | 3u << 2);
    bp(0xF3, 7u << 16 | 7u << 19);
    bp(0x41, 1u << 3 | 1u << 4);
    return 0;
}

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

/* Un vértice en píxeles del EFB (la pantalla suma 342) con color RGBA. Los
 * triángulos van como los deja Clipper (de frente): en la pantalla, con y
 * hacia abajo, en sentido antihorario */
static gx_rvtx_t px(float x, float y, float z, uint32_t rgba)
{
    gx_rvtx_t v;
    memset(&v, 0, sizeof v);
    v.scr[0] = x + 342;
    v.scr[1] = y + 342;
    v.scr[2] = z;
    v.proj[3] = 1;
    for (int i = 0; i < 4; i++)
        v.col[0][i] = (uint8_t)(rgba >> (24 - 8 * i));
    return v;
}

static void tri(gx_rvtx_t a, gx_rvtx_t b, gx_rvtx_t c)
{
    gx_raster_triangle(&sys.gx, &a, &b, &c);
}

static uint32_t color_at(uint32_t x, uint32_t y)
{
    return gx_efb_get_color(sys.gx.efb, sys.gx.bp[0x43] & 7u, x, y);
}

/* Dos triángulos que comparten la diagonal: cada píxel de [2, 5]² una vez,
 * sumando (ONE + ONE) */
static void test_raster_fill(void)
{
    CHECK(setup() == 0);
    bp(0x41, 1u | 1u << 5 | 1u << 8 | 1u << 3 | 1u << 4);
    tri(px(2, 2, 0, 0x10101010u), px(6, 6, 0, 0x10101010u), px(6, 2, 0, 0x10101010u));
    tri(px(2, 2, 0, 0x10101010u), px(2, 6, 0, 0x10101010u), px(6, 6, 0, 0x10101010u));
    uint32_t once = 0, more = 0, outside = 0;
    for (uint32_t y = 0; y < 10; y++)
        for (uint32_t x = 0; x < 10; x++) {
            const uint32_t c = color_at(x, y) >> 8;
            const int in = x >= 2 && x <= 5 && y >= 2 && y <= 5;
            if (c == 0x101010u && in)
                once++;
            else if (c != 0 && in)
                more++;
            else if (c != 0)
                outside++;
        }
    CHECK_EQ_U64(once, 16);
    CHECK_EQ_U64(more, 0);
    CHECK_EQ_U64(outside, 0);
    teardown();
}

/* Tijera de 0 a 2047 en x sin desplazamiento: dos rectángulos (0 y 1024), el
 * mejor según el área del viewport al final; al revés, ninguno */
static void test_raster_scissor(void)
{
    CHECK(setup() == 0);
    static const float vp[6] = { 512, -512, 16777216, 854, 854, 16777216 };
    uint8_t data[24];
    for (int i = 0; i < 6; i++) {
        uint32_t u;
        memcpy(&u, &vp[i], 4);
        for (int k = 0; k < 4; k++)
            data[4 * i + k] = (uint8_t)(u >> (24 - 8 * k));
    }
    gc_gx_load_xf(&sys.gx, 0x101A, 6, data);
    bp(0x20, 342);
    bp(0x21, (342 + 527) | 2047u << 12);
    bp(0x59, 171u << 10);
    gx_raster_scissor_changed(&sys.gx);
    const gx_raster_t *r = sys.gx.raster;
    CHECK_EQ_U64(r->nscissor, 2);
    CHECK_EQ_U64(r->scissor[0].x_off, 0);
    CHECK_EQ_U64(r->scissor[1].x_off, 1024);
    CHECK_EQ_U64(r->scissor[1].right, 640);
    CHECK_EQ_U64(r->scissor[1].y_off, 342);
    bp(0x20, 342 | 800u << 12);
    bp(0x21, (342 + 527) | 700u << 12);
    gx_raster_scissor_changed(&sys.gx);
    CHECK_EQ_U64(r->nscissor, 0);
    teardown();
}

/* Con la prueba de alfa que falla, la Z temprana igual escribe y la tardía no */
static void test_raster_early_late_z(void)
{
    CHECK(setup() == 0);
    bp(0x40, 1u | 7u << 1 | 1u << 4);                   /* siempre, con escritura */
    bp(0xF3, 0u << 16 | 7u << 19);                      /* nunca y siempre: falla */
    tri(px(0, 0, 0x123456, 0xFFFFFFFFu), px(0, 4, 0x123456, 0xFFFFFFFFu), px(4, 0, 0x123456, 0xFFFFFFFFu));
    CHECK_EQ_U64(gx_efb_get_depth(sys.gx.efb, 0, 1, 1), 0);
    CHECK_EQ_U64(color_at(1, 1), 0xFF);
    bp(0x43, 1u << 6);                                  /* temprana */
    tri(px(0, 0, 0x123456, 0xFFFFFFFFu), px(0, 4, 0x123456, 0xFFFFFFFFu), px(4, 0, 0x123456, 0xFFFFFFFFu));
    CHECK_EQ_U64(gx_efb_get_depth(sys.gx.efb, 0, 1, 1), 0x123456);
    CHECK_EQ_U64(color_at(1, 1), 0xFF);
    teardown();
}

/* SrcAlpha / InvSrcAlpha con alfa 0x80: los factores pasan a 0x81 y 0x7F */
static void test_raster_blend(void)
{
    CHECK(setup() == 0);
    gx_efb_t *e = sys.gx.efb;
    const uint32_t b = 1u | 5u << 5 | 4u << 8 | 1u << 3 | 1u << 4;
    gx_efb_write_t w = { 0, 1, 1, 0 };
    gx_efb_set_color(e, &w, 0, 0, 0x80808000u);
    bp(0x41, b);
    gx_efb_blend(e, sys.gx.bp, 0, 0, 0xFF000080u);
    CHECK_EQ_U64(color_at(0, 0), 0xBF3F3FFFu);
    /* Resta: dst - src con piso en 0 */
    bp(0x41, 1u | 1u << 11 | 1u << 3);
    gx_efb_blend(e, sys.gx.bp, 0, 0, 0x40FF1000u);
    CHECK_EQ_U64(color_at(0, 0), 0x7F002FFFu);
    /* XOR, y el alfa de destino en RGBA6 */
    bp(0x43, 1);
    bp(0x41, 2u | 6u << 12 | 1u << 3 | 1u << 4);
    bp(0x42, 0x100 | 0x84);
    gx_efb_set_color(e, &w, 1, 0, 0);
    gx_efb_blend(e, sys.gx.bp, 1, 0, 0xFC00FC00u);
    CHECK_EQ_U64(color_at(1, 0), 0xFF00FF86u);         /* 0x84 en 6 bits es 0x21: se lee 0x86 */
    /* Dither de 2 × 2 en RGBA6: en (1, 1) suma 1 */
    bp(0x42, 0);
    bp(0x41, 4u | 1u << 3 | 1u << 4);
    gx_efb_blend(e, sys.gx.bp, 1, 1, 0x7E7E7EFFu);
    CHECK_EQ_U64(color_at(1, 1) >> 8, 0x7D7D7Du);       /* (0x7E - 1 + 1) & 0xFC = 0x7C → 0x7D */
    teardown();
}

/* TEV: PREV = C0 × (1 - ras) + K0 × ras, con C0 por 0xE2/0xE3 y K0 por
 * 0xE0/0xE1 con el bit de constante */
static void test_raster_tev(void)
{
    CHECK(setup() == 0);
    bp(0xE2, 0x010 | 0x0FFu << 12);
    bp(0xE3, 0x030 | 0x020u << 12);
    bp(0xE0, 0x0A0 | 0x0FFu << 12 | 1u << 23);
    bp(0xE1, 0x0C0 | 0x0B0u << 12 | 1u << 23);
    bp(0xF6, 1u << 2 | 12u << 4);                       /* kcsel K0 */
    bp(0xC0, CC(2, 14, 10, 15));
    tri(px(0, 0, 0, 0x000000FFu), px(0, 4, 0, 0x000000FFu), px(4, 0, 0, 0x000000FFu));
    CHECK_EQ_U64(color_at(1, 1) >> 8, 0x102030u);
    tri(px(0, 0, 0, 0xFFFFFFFFu), px(0, 4, 0, 0xFFFFFFFFu), px(4, 0, 0, 0xFFFFFFFFu));
    CHECK_EQ_U64(color_at(1, 1) >> 8, 0xA0B0C0u);
    /* Sin recorte, C0 con -1 en azul llega como 0xFF */
    bp(0xE3, 0x7FF | 0x020u << 12);
    bp(0xC0, CC(15, 15, 15, 2) & ~(1u << 19));
    tri(px(0, 0, 0, 0xFFFFFFFFu), px(0, 4, 0, 0xFFFFFFFFu), px(4, 0, 0, 0xFFFFFFFFu));
    CHECK_EQ_U64(color_at(1, 1) >> 8, 0x1020FFu);
    teardown();
}

/* La caja: Set no marca un valor igual al que tiene en caché (el 0 de la
 * derecha y de abajo) y lo dibujado antes queda */
static void test_raster_bbox(void)
{
    CHECK(setup() == 0);
    gc_gx_t *gx = &sys.gx;
    gc_gx_bbox_update(gx, 600, 500);
    bp(0x55, 1023);
    bp(0x56, 1023);
    gc_gx_bbox_flush(gx);
    gc_gx_bbox_update(gx, 33, 17);
    CHECK_EQ_U64(gc_gx_bbox_get(gx, 0), 32);
    CHECK_EQ_U64(gc_gx_bbox_get(gx, 1), 601);
    CHECK_EQ_U64(gc_gx_bbox_get(gx, 2), 16);
    CHECK_EQ_U64(gc_gx_bbox_get(gx, 3), 501);
    /* Escrito sin dibujar: lo pendiente */
    bp(0x55, 10 | 300u << 10);
    CHECK_EQ_U64(gc_gx_bbox_get(gx, 1), 300);
    teardown();
}

/* El CPU lee color (ARGB, con el modo de alfa del PE) y Z; fuera, 0 */
static void test_raster_peek(void)
{
    CHECK(setup() == 0);
    gx_efb_write_t w = { 1, 1, 1, 1 };
    bp(0x43, 1);
    gx_efb_set_color(sys.gx.efb, &w, 3, 2, 0xFF8040FCu);
    gx_efb_set_depth(sys.gx.efb, &w, 3, 2, 0xABCDEF);
    const uint32_t a = 0xC8000000u | 3u << 2 | 2u << 12;
    CHECK_EQ_U64(gc_read32(&sys.mem, a), 0x00FF8241u);
    sys.pe.alpharead = 1;
    CHECK_EQ_U64(gc_read32(&sys.mem, a), 0xFFFF8241u);
    sys.pe.alpharead = 2;
    CHECK_EQ_U64(gc_read32(&sys.mem, a), 0xFFFF8241u);
    CHECK_EQ_U64(gc_read16(&sys.mem, a), 0x8241u);
    CHECK_EQ_U64(gc_read32(&sys.mem, a | 0x400000u), 0xABCDEF);
    CHECK_EQ_U64(gc_read64(&sys.mem, a | 0x400000u), 0xABCDEF);
    CHECK_EQ_U64(gc_read32(&sys.mem, 0xC8000000u | 700u << 2), 0);
    CHECK_EQ_U64(gc_read32(&sys.mem, a | 0x800000u), 0);
    gc_write32(&sys.mem, a, 0);                         /* no hace nada */
    CHECK_EQ_U64(gc_read32(&sys.mem, a | 0x400000u), 0xABCDEF);
    CHECK_EQ_U64(sys.mem.bad, 0);
    teardown();
}

static uint64_t dbits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

/* El 2^x de la niebla: exacto en los enteros, redondeado al más cercano en el
 * resto (en -0x1.480e76p-21 glibc da un double menos; el float es el mismo) */
static void test_raster_exp2(void)
{
    CHECK_EQ_U64(dbits(gx_exp2(-0.0f)), dbits(1.0));
    CHECK_EQ_U64(dbits(gx_exp2(-1.0f)), dbits(0.5));
    CHECK_EQ_U64(dbits(gx_exp2(-8.0f)), dbits(0x1p-8));
    CHECK_EQ_U64(dbits(gx_exp2(-0.5f)), dbits(0x1.6a09e667f3bcdp-1));
    CHECK_EQ_U64(dbits(gx_exp2(-0x1p-149f)), dbits(1.0));
    CHECK_EQ_U64(dbits(gx_exp2(-0x1.480e76p-21f)), dbits(0x1.fffff1c9bce6bp-1));
    CHECK_EQ_U64(dbits(gx_exp2(-7.99999952f)), dbits(0x1.0000058b90cf2p-8));
    const double n = gx_exp2(NAN);
    CHECK(n != n);
}

const test_case_t raster_tests[] = {
    { "raster: convención de relleno sin píxeles dobles", test_raster_fill },
    { "raster: rectángulos de tijera con la vuelta por 1024", test_raster_scissor },
    { "raster: Z temprana y tardía con la prueba de alfa", test_raster_early_late_z },
    { "raster: mezcla, resta, lógica, alfa de destino y dither", test_raster_blend },
    { "raster: TEV con registros, constantes y sin recorte", test_raster_tev },
    { "raster: caja envolvente con la caché de Dolphin", test_raster_bbox },
    { "raster: el CPU lee el EFB", test_raster_peek },
    { "raster: 2^x de la niebla", test_raster_exp2 },
    { NULL, NULL },
};
