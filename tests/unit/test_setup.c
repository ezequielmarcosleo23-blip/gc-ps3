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
 * Fase 2, etapa C: armado y recorte (src/gx/setup.c). El orden de los
 * vértices de cada tipo de primitiva (IndexGenerator), el recorte con la
 * interpolación de colores, el culling y la división por w con el viewport.
 * Los casos generales los compara el DOL gx_raster contra Dolphin.
 */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/raster.h"
#include "test.h"

static gc_system_t sys;
static gx_rvtx_t tris[16][3];
static uint32_t ntris;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void capture(void *ctx, const gx_rvtx_t *const v[3], uint32_t ntex)
{
    (void)ctx;
    (void)ntex;
    if (ntris < 16)
        for (int k = 0; k < 3; k++)
            tris[ntris][k] = *v[k];
    ntris++;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static void xf_f(uint32_t reg, float f)
{
    sys.gx.xf[reg] = fbits(f);
}

/* Viewport identidad en x e y (ancho 1, origen 0) y z sin cambios */
static int setup(void)
{
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    sys.gx.triangle_cb = capture;
    ntris = 0;
    xf_f(0x101A, 1.0f);
    xf_f(0x101B, -1.0f);
    xf_f(0x101C, 1.0f);
    return 0;
}

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

/* Un vértice adentro, marcado por x (en la pantalla queda x) */
static gx_vtx_out_t vert(float x, float y)
{
    gx_vtx_out_t v;
    memset(&v, 0, sizeof v);
    v.proj[0] = x;
    v.proj[1] = y;
    v.proj[2] = -0.5f;
    v.proj[3] = 1.0f;
    return v;
}

/* n vértices en zigzag: con la tira, todos los triángulos quedan de frente */
static void feed(uint32_t type, uint32_t n)
{
    gx_setup_begin(&sys.gx, type);
    for (uint32_t k = 0; k < n; k++) {
        const gx_vtx_out_t v = vert(0.0625f * (float)k, (k & 1u) ? 0.0f : 0.5f);
        gx_setup_vertex(&sys.gx, &v);
    }
}

/* El vértice k de un triángulo: el índice de carga sale de x */
static int idx(uint32_t t, int k)
{
    return (int)(tris[t][k].scr[0] / 0.0625f + 0.5f);
}

/* El triángulo t es (a, b, c), o (a, c, b) si quedó de espaldas (Clipper los da vuelta) */
static int is_tri(uint32_t t, int a, int b, int c)
{
    return idx(t, 0) == a && ((idx(t, 1) == b && idx(t, 2) == c) || (idx(t, 1) == c && idx(t, 2) == b));
}

static void test_setup_order(void)
{
    CHECK(setup() == 0);
    /* Tira con culling (modo 2: descarta los que IsBackface da como de
     * espaldas): si el orden de los impares no se diera vuelta, la mitad se
     * descartaría */
    gc_gx_load_bp(&sys.gx, 0x00, 2u << 14);
    feed(3, 5);
    CHECK_EQ_U64(ntris, 3);
    CHECK(idx(0, 0) == 0 && idx(0, 1) == 1 && idx(0, 2) == 2);
    CHECK(idx(1, 0) == 1 && idx(1, 1) == 3 && idx(1, 2) == 2);
    CHECK(idx(2, 0) == 2 && idx(2, 1) == 3 && idx(2, 2) == 4);
    gc_gx_load_bp(&sys.gx, 0x00, 0);

    ntris = 0;
    feed(4, 4);                                         /* abanico */
    CHECK_EQ_U64(ntris, 2);
    CHECK(is_tri(0, 0, 1, 2) && is_tri(1, 0, 2, 3));

    ntris = 0;
    feed(0, 7);                                         /* cuadrados: 4 y 3 sueltos */
    CHECK_EQ_U64(ntris, 3);
    CHECK(is_tri(0, 0, 1, 2) && is_tri(1, 0, 2, 3) && is_tri(2, 4, 5, 6));

    ntris = 0;
    feed(2, 6);                                         /* triángulos */
    CHECK_EQ_U64(ntris, 2);
    CHECK(is_tri(1, 3, 4, 5));
    teardown();
}

/* Líneas y puntos: dos triángulos cada uno, con el ancho y el tamaño de BP 0x22 */
static void test_setup_lines_points(void)
{
    CHECK(setup() == 0);
    gc_gx_load_bp(&sys.gx, 0x22, 12 | 24u << 8);         /* línea de 1 (medio ancho 1), punto de 2 */
    feed(6, 3);                                         /* tira de líneas: 2 líneas */
    CHECK_EQ_U64(ntris, 4);
    /* La primera línea va de x 0 a 0,0625 (y de 0,5 a 0, en la pantalla de -0,5 a 0):
     * más vertical y hacia abajo, px = 1 */
    CHECK_EQ_U64(fbits(tris[0][0].scr[0]), fbits(0.0625f - 1.0f));
    CHECK_EQ_U64(fbits(tris[0][2].scr[0]), fbits(0.0f + 1.0f));

    ntris = 0;
    feed(7, 1);                                         /* punto */
    CHECK_EQ_U64(ntris, 2);
    CHECK_EQ_U64(fbits(tris[0][0].scr[0]), fbits(-2.0f));  /* ll */
    CHECK_EQ_U64(fbits(tris[0][1].scr[1]), fbits(-0.5f + 2.0f));   /* ul: el punto está en y 0,5 */
    teardown();
}

/* Recorte contra x = w: el vértice nuevo interpola colores con t en 1/256 */
static void test_setup_clip_lerp(void)
{
    CHECK(setup() == 0);
    gx_vtx_out_t a = vert(0, 0), b = vert(3, 0), c = vert(0, 0.5f);
    memcpy(a.col[0], (const uint8_t[]){ 200, 0, 7, 255 }, 4);
    memcpy(b.col[0], (const uint8_t[]){ 0, 100, 7, 0 }, 4);
    gx_setup_begin(&sys.gx, 2);
    gx_setup_vertex(&sys.gx, &a);
    gx_setup_vertex(&sys.gx, &b);
    gx_setup_vertex(&sys.gx, &c);
    /* El borde a-b corta x = 1 en t = 1/3 desde b: (1 - 3) / ((1 - 3) - (1 - 0)) */
    CHECK(ntris >= 1);
    const gx_rvtx_t *n = NULL;
    for (uint32_t t = 0; t < ntris && !n; t++)
        for (int k = 0; k < 3; k++)
            if (tris[t][k].scr[0] == 1.0f && tris[t][k].scr[1] == 0.0f)
                n = &tris[t][k];
    CHECK(n != NULL);
    /* t_int = (u16)(0,6667 × 256) = 170: desde b, b + ((a - b) × 170) >> 8 */
    CHECK_EQ_U64(n->col[0][0], 0 + ((200 * 170) >> 8));
    CHECK_EQ_U64(n->col[0][1], (uint8_t)(100 + ((-100 * 170) >> 8)));
    CHECK_EQ_U64(n->col[0][2], 7);
    teardown();
}

/* Culling: el triángulo antihorario es de frente con el viewport hacia abajo */
static void test_setup_culling(void)
{
    static const struct { uint32_t cull; uint32_t want; } c[4] = { { 0, 2 }, { 1, 1 }, { 2, 1 }, { 3, 0 } };
    CHECK(setup() == 0);
    for (int i = 0; i < 4; i++) {
        gc_gx_load_bp(&sys.gx, 0x00, c[i].cull << 14);
        ntris = 0;
        gx_vtx_out_t a = vert(0, 0), b = vert(0.5f, 0), d = vert(0, 0.5f);
        gx_setup_begin(&sys.gx, 2);
        gx_setup_vertex(&sys.gx, &a);
        gx_setup_vertex(&sys.gx, &b);
        gx_setup_vertex(&sys.gx, &d);
        gx_setup_vertex(&sys.gx, &a);
        gx_setup_vertex(&sys.gx, &d);
        gx_setup_vertex(&sys.gx, &b);
        CHECK_EQ_U64(ntris, c[i].want);
    }
    teardown();
}

/* Afuera del todo por un mismo plano: no llega nada */
static void test_setup_reject(void)
{
    CHECK(setup() == 0);
    gx_vtx_out_t a = vert(2, 0), b = vert(3, 0), d = vert(2, 0.5f);
    gx_setup_begin(&sys.gx, 2);
    gx_setup_vertex(&sys.gx, &a);
    gx_setup_vertex(&sys.gx, &b);
    gx_setup_vertex(&sys.gx, &d);
    CHECK_EQ_U64(ntris, 0);
    teardown();
}

const test_case_t setup_tests[] = {
    { "setup: orden de tiras, abanicos y cuadrados", test_setup_order },
    { "setup: líneas y puntos como triángulos", test_setup_lines_points },
    { "setup: recorte e interpolación de colores", test_setup_clip_lerp },
    { "setup: culling", test_setup_culling },
    { "setup: triángulo afuera del todo", test_setup_reject },
    { NULL, NULL },
};
