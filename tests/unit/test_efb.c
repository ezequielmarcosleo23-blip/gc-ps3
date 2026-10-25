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
 * Fase 2, etapa C: el EFB (src/gx/efb.c) y el disparo de las copias
 * (src/gx/copy.c): formatos de píxel, máscaras de escritura, borrado y la
 * línea del volcado. Los valores salen de SWEfbInterface.cpp, EfbCopy.cpp y
 * BPStructs.cpp de Dolphin cf5cb7ef.
 */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "test.h"

static gc_system_t sys;
static char last[256];
static int lines;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void capture(void *ctx, const char *text)
{
    (void)ctx;
    strncpy(last, text, sizeof last - 1);
    lines++;
}

static int setup(void)
{
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    sys.gx.copy_cb = capture;
    lines = 0;
    last[0] = '\0';
    return 0;
}

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

static void bp(uint8_t reg, uint32_t v)
{
    gc_gx_load_bp(&sys.gx, reg, v);
}

static void test_efb_formats(void)
{
    static gx_efb_t e;
    memset(&e, 0, sizeof e);
    gx_efb_write_t w = { GX_PF_RGB8_Z24, 1, 1, 1 };
    gx_efb_set_color(&e, &w, 3, 4, 0x12345678u);
    CHECK_EQ_U64(e.px[3 + 4 * GX_EFB_WIDTH], 0x123456u);
    CHECK_EQ_U64(gx_efb_get_color(&e, GX_PF_RGB8_Z24, 3, 4), 0x123456FFu);

    /* RGBA6: 6 bits por canal; al leer, 6 a 8 replicando los de arriba */
    w.pf = GX_PF_RGBA6_Z24;
    gx_efb_set_color(&e, &w, 0, 0, 0xFF8040C4u);
    CHECK_EQ_U64(e.px[0], 0x3F << 18 | 0x20 << 12 | 0x10 << 6 | 0x31);
    CHECK_EQ_U64(gx_efb_get_color(&e, GX_PF_RGBA6_Z24, 0, 0), 0xFF8241C7u);
    /* Solo el color: el alfa queda */
    w.alpha_update = 0;
    gx_efb_set_color(&e, &w, 0, 0, 0x00000000u);
    CHECK_EQ_U64(e.px[0], 0x31u);
    /* Solo el alfa */
    w.color_update = 0;
    w.alpha_update = 1;
    gx_efb_set_color(&e, &w, 0, 0, 0xFFFFFF40u);
    CHECK_EQ_U64(e.px[0], 0x10u);
    /* En RGB8 el alfa no se escribe */
    w.pf = GX_PF_RGB8_Z24;
    gx_efb_set_color(&e, &w, 3, 4, 0x000000FFu);
    CHECK_EQ_U64(e.px[3 + 4 * GX_EFB_WIDTH], 0x123456u);

    /* Z de 24 bits, solo con la máscara; los formatos YUV no tienen */
    w.z_update = 1;
    gx_efb_set_depth(&e, &w, 1, 1, 0xABCDEF12u);
    CHECK_EQ_U64(gx_efb_get_depth(&e, GX_PF_Z24, 1, 1), 0xCDEF12u);
    w.z_update = 0;
    gx_efb_set_depth(&e, &w, 1, 1, 7);
    CHECK_EQ_U64(gx_efb_get_depth(&e, GX_PF_RGB8_Z24, 1, 1), 0xCDEF12u);
    CHECK_EQ_U64(gx_efb_get_depth(&e, GX_PF_Y8, 1, 1), 0);
    CHECK_EQ_U64(gx_efb_get_color(&e, GX_PF_YUV420, 3, 4), 0);
}

/* El EFB en cero: el CRC que anota Dolphin cuando no se dibujó nada */
static void test_efb_crc(void)
{
    CHECK(setup() == 0);
    uint32_t c, z;
    gx_efb_crc(sys.gx.efb, 0, 0, GX_EFB_WIDTH, GX_EFB_HEIGHT, &c, &z);
    CHECK_EQ_U64(c, 0xc5b034a6u);
    CHECK_EQ_U64(z, 0xc5b034a6u);
    gc_gx_efb_final(&sys.gx);
    CHECK(strcmp(last, "efb c5b034a6 c5b034a6") == 0);
    teardown();
}

static void test_efb_clear(void)
{
    CHECK(setup() == 0);
    bp(0x43, GX_PF_RGBA6_Z24);
    bp(0x41, 1u << 3 | 1u << 4);                        /* color y alfa */
    bp(0x40, 1u << 4);                                  /* Z */
    bp(0x4F, 0x80FF);                                   /* A = 0x80, R = 0xFF */
    bp(0x50, 0x4020);                                   /* G = 0x40, B = 0x20 */
    bp(0x51, 0x123456);
    bp(0x49, 10 | 20u << 10);
    bp(0x4A, 3 | 1u << 10);                             /* 4 × 2 */
    bp(0x4D, 0);                                        /* sin copia */
    bp(0x52, 1u << 11);                                 /* borrar */
    CHECK_EQ_U64(lines, 1);
    CHECK(strncmp(last, "c 1 000800 src 10 20 4 2 dst 00000000 0 efb ", 44) == 0);
    CHECK(strstr(last, " ram 0 00000000") != NULL);
    CHECK_EQ_U64(gx_efb_get_color(sys.gx.efb, GX_PF_RGBA6_Z24, 13, 21), 0xFF412082u);
    CHECK_EQ_U64(gx_efb_get_depth(sys.gx.efb, GX_PF_Z24, 10, 20), 0x123456u);
    CHECK_EQ_U64(gx_efb_get_depth(sys.gx.efb, GX_PF_Z24, 14, 20), 0);
    CHECK_EQ_U64(gx_efb_get_depth(sys.gx.efb, GX_PF_Z24, 10, 22), 0);

    /* RGB8 sin color ni Z: el alfa no cuenta y no se borra nada */
    bp(0x43, GX_PF_RGB8_Z24);
    bp(0x41, 1u << 4);
    bp(0x40, 0);
    bp(0x49, 0);
    bp(0x52, 1u << 11);
    CHECK_EQ_U64(gx_efb_get_depth(sys.gx.efb, GX_PF_Z24, 0, 0), 0);
    teardown();
}

/* El rectángulo se recorta al EFB; si empieza afuera, ni copia ni borrado */
static void test_efb_copy_bounds(void)
{
    CHECK(setup() == 0);
    bp(0x41, 1u << 3);
    bp(0x40, 1u << 4);
    bp(0x51, 0x111111);
    bp(0x49, 630 | 520u << 10);
    bp(0x4A, 19 | 19u << 10);                           /* 20 × 20: se pasa */
    bp(0x52, 1u << 11);
    CHECK(strncmp(last, "c 0 000800 src 630 520 20 20 dst 00000000 0 efb ", 48) == 0);
    /* El borrado llega hasta el borde */
    CHECK_EQ_U64(gx_efb_get_depth(sys.gx.efb, GX_PF_Z24, 639, 527), 0x111111u);
    bp(0x49, 640 | 0u << 10);
    bp(0x51, 0x222222);
    bp(0x52, 1u << 11);
    CHECK(strcmp(last, "c 0 000800 src 640 0 20 20 dst 00000000 0 fuera") == 0);
    CHECK_EQ_U64(gx_efb_get_depth(sys.gx.efb, GX_PF_Z24, 639, 0), 0);
    teardown();
}

/* Lo que ocupa una copia en la RAM: bloques de su formato, por filas */
static void test_efb_copy_size(void)
{
    CHECK(setup() == 0);
    bp(0x49, 0);
    bp(0x4A, 99 | 49u << 10);                           /* 100 × 50 */
    bp(0x4B, 0x1000);
    bp(0x4D, 0x40);                                     /* paso 2048 */
    bp(0x52, 6u << 4);                                  /* RGBA8: 25 × 13 bloques de 64 */
    CHECK(strstr(last, " ram 20800 ") != NULL);
    bp(0x52, 1u << 3 | 1u << 9);                        /* R8 (Z8H), a la mitad: 50 × 25 */
    CHECK(strstr(last, " ram 1568 ") != NULL);          /* 7 bloques de 8 × 4, 7 filas */
    /* Al XFB: 16 píxeles por bloque, una fila por línea con la escala */
    bp(0x4E, 0x80);                                     /* 0,5 */
    bp(0x52, 1u << 14);
    CHECK(strstr(last, " ram 5600 ") != NULL);          /* 7 bloques × 25 líneas (1 + 49 × 0,5) */
    CHECK(strncmp(last, "c 0 004000 src 0 0 100 50 dst 00020000 2048 efb ", 48) == 0);
    teardown();
}

const test_case_t efb_tests[] = {
    { "efb: formatos de píxel y máscaras", test_efb_formats },
    { "efb: CRC del EFB vacío", test_efb_crc },
    { "efb: borrado al copiar", test_efb_clear },
    { "efb: copia que se pasa del EFB", test_efb_copy_bounds },
    { "efb: tamaño de la copia en la RAM", test_efb_copy_size },
    { NULL, NULL },
};
