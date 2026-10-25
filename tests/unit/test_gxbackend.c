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
 * Fase 2, etapa D1: la interfaz del backend (src/gx/backend.h). Cada caso se
 * dibuja con el backend por software, que dibuja en el acto, y con el de
 * lotes, que dibuja recién al vaciar: el EFB tiene que salir igual, y eso
 * pasa solo si el frente vacía en cada punto que hace falta. Los casos
 * cubren lo que los DOLs no separan: el viewport de XF (sin BP en el medio),
 * el fin del tramo de la GPU con la CPU cambiando una textura, la lectura del
 * EFB por el CPU y el orden de los triángulos descartados para zfreeze.
 */
#include <string.h>

#include "core/crc32.h"
#include "core/log.h"
#include "core/system.h"
#include "gx/raster.h"
#include "test.h"

static gc_system_t sys;
static float vp_wd, vp_ht, vp_xo, vp_yo;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void bp(uint8_t reg, uint32_t v)
{
    gc_gx_load_bp(&sys.gx, reg, v);
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* El viewport por un comando de XF: ancho, alto, rango de Z, origen y lejos */
static void viewport(float wd, float ht, float xo, float yo)
{
    const float v[6] = { wd, ht, 16777215.0f, xo, yo, 16777215.0f };
    uint8_t d[24];
    for (int i = 0; i < 6; i++)
        be32(d + 4 * i, fbits(v[i]));
    gc_gx_load_xf(&sys.gx, 0x101A, 6, d);
    vp_wd = wd;
    vp_ht = ht;
    vp_xo = xo;
    vp_yo = yo;
}

/* El mismo viewport, por una carga indexada de XF (el arreglo 12 en la RAM) */
static void viewport_indexed(float wd, float ht, float xo, float yo)
{
    const float v[6] = { wd, ht, 16777215.0f, xo, yo, 16777215.0f };
    uint8_t *ram = gc_ram_phys(&sys.mem, 0x20000, 24);
    for (int i = 0; i < 6; i++)
        be32(ram + 4 * i, fbits(v[i]));
    sys.gx.cp.array_base[12] = 0x20000;
    sys.gx.cp.array_stride[12] = 24;
    gc_gx_load_xf_indexed(&sys.gx, 12, 0, 0x101A, 6);
    vp_wd = wd;
    vp_ht = ht;
    vp_xo = xo;
    vp_yo = yo;
}

#define CC(a, b, c, d) ((uint32_t)(a) << 12 | (uint32_t)(b) << 8 | (uint32_t)(c) << 4 | (uint32_t)(d) | 1u << 19)
#define AC(a, b, c, d) ((uint32_t)(a) << 13 | (uint32_t)(b) << 10 | (uint32_t)(c) << 7 | (uint32_t)(d) << 4 | 1u << 19)

/* Una etapa que pasa el color, tijera en todo el EFB, sin prueba de alfa,
 * RGB8 sin Z, escritura de color y alfa, viewport de 640 × 480 */
static int setup(const gx_backend_t *be)
{
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    if (gc_gx_set_backend(&sys.gx, be) != 0) {
        gc_system_free(&sys);
        return -1;
    }
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
    viewport(320.0f, -240.0f, 342.0f + 320.0f, 342.0f + 240.0f);
    return 0;
}

/* Un vértice en píxeles del EFB con el viewport de ahora, Z de 0 a 1, color
 * RGBA y coordenada de textura en téxeles */
static gx_vtx_out_t vert(float x, float y, float z, uint32_t rgba, float s, float t)
{
    gx_vtx_out_t v;
    memset(&v, 0, sizeof v);
    v.proj[0] = (342.0f + x - vp_xo) / vp_wd;
    v.proj[1] = (342.0f + y - vp_yo) / vp_ht;
    v.proj[2] = z - 1.0f;
    v.proj[3] = 1.0f;
    for (int i = 0; i < 4; i++)
        v.col[0][i] = (uint8_t)(rgba >> (24 - 8 * i));
    v.tex[0][0] = s;
    v.tex[0][1] = t;
    v.tex[0][2] = 1.0f;
    return v;
}

/* Un triángulo suelto (GX_TRIANGLES) */
static void tri(gx_vtx_out_t a, gx_vtx_out_t b, gx_vtx_out_t c)
{
    gx_setup_begin(&sys.gx, 2);
    gx_setup_vertex(&sys.gx, &a);
    gx_setup_vertex(&sys.gx, &b);
    gx_setup_vertex(&sys.gx, &c);
}

/* Un cuadrado de dos triángulos, de frente en la pantalla */
static void quad(float x0, float y0, float x1, float y1, float z, uint32_t rgba)
{
    tri(vert(x0, y0, z, rgba, 0, 0), vert(x0, y1, z, rgba, 0, 8), vert(x1, y0, z, rgba, 8, 0));
    tri(vert(x1, y0, z, rgba, 8, 0), vert(x0, y1, z, rgba, 0, 8), vert(x1, y1, z, rgba, 8, 8));
}

typedef struct result {
    uint32_t         color, z;
    gx_lotes_stats_t lotes;
    uint64_t         triangles;
    uint32_t         peek;
} result_t;

static result_t finish(void)
{
    result_t r;
    memset(&r, 0, sizeof r);
    gc_gx_efb_sync(&sys.gx);
    gx_efb_crc(sys.gx.efb, 0, 0, GX_EFB_WIDTH, GX_EFB_HEIGHT, &r.color, &r.z);
    gx_lotes_stats(&sys.gx, &r.lotes);
    r.triangles = sys.gx.triangles;
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
    return r;
}

/* Corre el caso con los dos backends: el mismo EFB */
static void both(void (*scene)(result_t *), result_t *sw, result_t *lotes)
{
    result_t a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    CHECK(setup(&gx_backend_sw) == 0);
    scene(&a);
    const uint32_t peek_a = a.peek;
    a = finish();
    a.peek = peek_a;
    CHECK(setup(&gx_backend_sw_lotes) == 0);
    scene(&b);
    const uint32_t peek_b = b.peek;
    b = finish();
    b.peek = peek_b;
    CHECK_EQ_U64(b.color, a.color);
    CHECK_EQ_U64(b.z, a.z);
    CHECK_EQ_U64(b.peek, a.peek);
    CHECK_EQ_U64(b.triangles, a.triangles);
    *sw = a;
    *lotes = b;
}

/* BP: la mezcla cambia entre dos cuadrados; los dos triángulos de cada uno
 * van en el mismo lote */
static void scene_bp(result_t *r)
{
    (void)r;
    quad(10, 10, 60, 60, 0.5f, 0xC0202080u);
    bp(0x41, 1u | 1u << 5 | 1u << 8 | 1u << 3 | 1u << 4);      /* ONE + ONE */
    quad(30, 30, 90, 90, 0.5f, 0x20C02080u);
}

static void test_gxbe_bp(void)
{
    result_t a, b;
    both(scene_bp, &a, &b);
    CHECK_EQ_U64(b.lotes.batches, 2);
    CHECK_EQ_U64(b.lotes.triangles, 4);
    CHECK_EQ_U64(b.lotes.max, 2);
    CHECK(a.color != 0);
}

/* Niebla de rango: lee el ancho del viewport al dibujar. Cambiarlo por XF
 * (sin escribir BP) tiene que vaciar el lote: por un comando y por una carga
 * indexada */
static void fog_setup(void)
{
    bp(0xEE, 127u << 11);                                       /* A = 1 */
    bp(0xF1, 1u << 20 | 2u << 21);                              /* ortográfica, lineal, C = 0 */
    bp(0xF2, 0xFFFFFFu);                                        /* blanca */
    bp(0xE8, 1u << 10 | (342u + 100u));                        /* rango, centro en x = 100 */
    for (uint8_t k = 0xE9; k <= 0xED; k++)
        bp(k, 64u | 64u << 12);                                 /* k = 1 */
}

static void scene_viewport(result_t *r)
{
    (void)r;
    fog_setup();
    quad(0, 10, 40, 50, 0.25f, 0x000000FFu);
    viewport(40.0f, -240.0f, 342.0f + 320.0f, 342.0f + 240.0f);   /* se ve de x = 280 a 360 */
    quad(290, 10, 330, 50, 0.25f, 0x000000FFu);
    viewport_indexed(640.0f, -240.0f, 342.0f + 320.0f, 342.0f + 240.0f);
    quad(400, 10, 440, 50, 0.25f, 0x000000FFu);
}

static void test_gxbe_viewport(void)
{
    result_t a, b;
    both(scene_viewport, &a, &b);
    CHECK_EQ_U64(b.lotes.batches, 3);
    CHECK_EQ_U64(a.triangles, 6);
}

/* La CPU cambia una textura entre dos tramos de la GPU sin tocar BP: el fin
 * del tramo tiene que vaciar el lote */
static void scene_texture(result_t *r)
{
    (void)r;
    uint8_t *tex = gc_ram_phys(&sys.mem, 0x10000, 32);
    for (int i = 0; i < 32; i++)
        tex[i] = (uint8_t)(i * 8);
    bp(0x00, 1u | 1u << 4);                                     /* 1 coordenada, 1 canal */
    bp(0x28, 1u << 6);                                          /* etapa 0: textura 0 con la coordenada 0 */
    bp(0xC0, CC(15, 15, 15, 8));                                /* el color de la textura */
    bp(0x80, 0);                                                /* recortar, más cercano */
    bp(0x84, 0);
    bp(0x88, 7u | 3u << 10 | 1u << 20);                         /* I8 de 8 × 4 */
    bp(0x94, 0x10000u >> 5);
    quad(10, 10, 26, 26, 0.5f, 0xFFFFFFFFu);
    gc_gx_run_gpu_on_cpu(&sys.gx, 0);                           /* sin FIFO: solo termina el tramo */
    for (int i = 0; i < 32; i++)
        tex[i] = (uint8_t)(255 - i * 8);
    quad(40, 10, 56, 26, 0.5f, 0xFFFFFFFFu);
}

static void test_gxbe_texture(void)
{
    result_t a, b;
    both(scene_texture, &a, &b);
    CHECK_EQ_U64(b.lotes.batches, 2);
}

/* El CPU lee el EFB con un lote pendiente: tiene que ver lo dibujado */
static void scene_peek(result_t *r)
{
    quad(10, 10, 30, 30, 0.5f, 0x4080C0FFu);
    r->peek = gc_read32(&sys.mem, 0xC8000000u | 20u << 2 | 20u << 12);
    quad(20, 20, 40, 40, 0.5f, 0x102030FFu);
}

static void test_gxbe_peek(void)
{
    result_t a, b;
    both(scene_peek, &a, &b);
    CHECK_EQ_U64(a.peek, 0x004080C0u);
}

/* zfreeze: el triángulo descartado deja su pendiente de Z en orden, en el
 * mismo lote que los que se dibujan */
static void scene_zfreeze(result_t *r)
{
    (void)r;
    bp(0x40, 1u | 7u << 1 | 1u << 4);                           /* Z siempre, con escritura */
    bp(0x00, 1u << 4 | 2u << 14);                               /* culling: los de espaldas para IsBackface */
    tri(vert(10, 10, 0.2f, 0xFF0000FFu, 0, 0), vert(10, 50, 0.6f, 0xFF0000FFu, 0, 0),
        vert(50, 10, 0.2f, 0xFF0000FFu, 0, 0));
    tri(vert(60, 10, 0.9f, 0x00FF00FFu, 0, 0), vert(100, 10, 0.1f, 0x00FF00FFu, 0, 0),
        vert(60, 50, 0.1f, 0x00FF00FFu, 0, 0));                 /* de espaldas: descartado */
    bp(0x00, 1u << 4 | 2u << 14 | 1u << 19);                    /* zfreeze */
    quad(110, 10, 150, 50, 0.5f, 0x0000FFFFu);
}

static void test_gxbe_zfreeze(void)
{
    result_t a, b;
    both(scene_zfreeze, &a, &b);
    CHECK_EQ_U64(a.triangles, 3);
    CHECK_EQ_U64(b.lotes.batches, 2);
}

/* Elegir el backend: el de lotes se puede poner y sacar */
static void test_gxbe_select(void)
{
    gc_log_set_sink(quiet);
    CHECK(gc_system_init(&sys) == 0);
    CHECK(sys.gx.be == &gx_backend_sw);
    gx_lotes_stats_t s;
    CHECK(gx_lotes_stats(&sys.gx, &s) != 0);
    CHECK(gc_gx_set_backend(&sys.gx, &gx_backend_sw_lotes) == 0);
    CHECK(sys.gx.be_state != NULL);
    CHECK(gx_lotes_stats(&sys.gx, &s) == 0 && s.batches == 0);
    CHECK(gc_gx_set_backend(&sys.gx, &gx_backend_sw) == 0);
    CHECK(sys.gx.be_state == NULL);
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

const test_case_t gxbackend_tests[] = {
    { "gx backend: se elige y el de lotes cuenta", test_gxbe_select },
    { "gx backend: lotes que vacía BP", test_gxbe_bp },
    { "gx backend: lotes que vacía el viewport de XF (niebla de rango)", test_gxbe_viewport },
    { "gx backend: lotes que vacía el fin del tramo (textura cambiada por la CPU)", test_gxbe_texture },
    { "gx backend: lotes con el CPU leyendo el EFB", test_gxbe_peek },
    { "gx backend: lotes con un descartado antes de zfreeze", test_gxbe_zfreeze },
    { NULL, NULL },
};
