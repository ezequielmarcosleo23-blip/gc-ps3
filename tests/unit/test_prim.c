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
 * Fase 2, etapa B: las primitivas (src/gx/prim.c) con comandos de FIFO armados
 * a mano: qué vértices se transforman, la caché de normales, XF y BP que no
 * coinciden, y que los cambios de estado lleguen a la primitiva siguiente.
 */
#include <math.h>
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/xform.h"
#include "test.h"

static gc_system_t sys;

#define MAXV 64
static gx_vtx_out_t got[MAXV];
static uint32_t ngot, got_ntex;

static uint8_t cmd[4096];
static uint32_t clen;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void capture(void *ctx, const struct gx_vtx_out *v, uint32_t ntex)
{
    (void)ctx;
    if (ngot < MAXV)
        got[ngot] = *v;
    ngot++;
    got_ntex = ntex;
}

static void put8(uint32_t v) { cmd[clen++] = (uint8_t)v; }
static void put16(uint32_t v) { put8(v >> 8); put8(v); }
static void put32(uint32_t v) { put16(v >> 16); put16(v); }

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static void cp(uint8_t reg, uint32_t v) { put8(0x08); put8(reg); put32(v); }
static void bp(uint8_t reg, uint32_t v) { put8(0x61); put32((uint32_t)reg << 24 | (v & 0xFFFFFFu)); }

static void xf(uint16_t addr, uint32_t n, const uint32_t *v)
{
    put8(0x10);
    put32((n - 1) << 16 | addr);
    for (uint32_t k = 0; k < n; k++)
        put32(v[k]);
}

static void xf1(uint16_t addr, uint32_t v) { xf(addr, 1, &v); }

static void run(void)
{
    ngot = 0;
    CHECK_EQ_U64(gc_gx_run_commands(&sys.gx, cmd, clen), clen);
    clen = 0;
}

/* Identidad en la matriz 0 y en la normal 0, posición directa u8 XY en el
 * VAT 0, proyección ortográfica identidad, sin coordenadas ni canales */
static int setup(void)
{
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    sys.gx.vertex_cb = capture;
    clen = 0;
    const uint32_t m[12] = { fbits(1), 0, 0, 0, 0, fbits(1), 0, 0, 0, 0, fbits(1), 0 };
    xf(0x000, 12, m);
    const uint32_t n[9] = { fbits(1), 0, 0, 0, fbits(1), 0, 0, 0, fbits(1) };
    xf(0x400, 9, n);
    const uint32_t proj[7] = { fbits(1), 0, fbits(1), 0, fbits(1), 0, 1 };
    xf(0x1020, 7, proj);
    cp(0x50, 1u << 9);                                  /* posición directa */
    cp(0x60, 0);
    cp(0x70, 0);                                        /* XY, u8 */
    return 0;
}

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

/* n vértices u8 XY: (k, 0) */
static void prim(uint8_t op, uint32_t n)
{
    put8(op);
    put16(n);
    for (uint32_t k = 0; k < n; k++) {
        put8(k);
        put8(0);
    }
}

static void test_prim_referenced(void)
{
    static const struct { uint8_t op; uint32_t want[10]; } t[] = {
        { 0x80, { 0, 0, 0, 3, 4, 4, 4, 7, 8, 8 } },       /* cuadrados: 4 en 4, y 3 sueltos */
        { 0x88, { 0, 0, 0, 3, 4, 4, 4, 7, 8, 8 } },
        { 0x90, { 0, 0, 0, 3, 3, 3, 6, 6, 6, 9 } },       /* triángulos */
        { 0x98, { 0, 0, 0, 3, 4, 5, 6, 7, 8, 9 } },       /* tira */
        { 0xA0, { 0, 0, 0, 3, 4, 5, 6, 7, 8, 9 } },       /* abanico */
        { 0xA8, { 0, 0, 2, 2, 4, 4, 6, 6, 8, 8 } },       /* líneas */
        { 0xB0, { 0, 0, 2, 3, 4, 5, 6, 7, 8, 9 } },       /* tira de líneas */
        { 0xB8, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },       /* puntos */
    };
    CHECK(setup() == 0);
    run();
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        for (uint32_t n = 0; n < 10; n++) {
            prim(t[i].op, n);
            run();
            if (ngot != t[i].want[n])
                fprintf(stderr, "  primitiva 0x%02x con %u: %u\n", t[i].op, (unsigned)n, (unsigned)ngot);
            CHECK_EQ_U64(ngot, t[i].want[n]);
            /* Los primeros, en orden */
            for (uint32_t k = 0; k < ngot; k++)
                CHECK_EQ_U64(fbits(got[k].mv[0]), fbits((float)k));
        }
    teardown();
}

/* Un índice de posición 0xFF descarta el vértice antes de contar */
static void test_prim_skip(void)
{
    CHECK(setup() == 0);
    cp(0x50, 2u << 9);                                  /* posición con índice de 8 bits */
    cp(0xA0, 0x1000);                                   /* arreglo 0 en 0x1000, de a 2 bytes */
    cp(0xB0, 2);
    run();
    for (uint32_t k = 0; k < 8; k++) {
        gc_write8(&sys.mem, 0x80001000u + 2 * k, (uint8_t)(10 + k));
        gc_write8(&sys.mem, 0x80001001u + 2 * k, 0);
    }
    /* 4 vértices, uno descartado: queda un triángulo con 0, 2 y 3 */
    put8(0x90);
    put16(4);
    put8(0);
    put8(0xFF);
    put8(2);
    put8(3);
    run();
    CHECK_EQ_U64(ngot, 3);
    CHECK_EQ_U64(fbits(got[1].mv[0]), fbits(12.0f));
    CHECK_EQ_U64(fbits(got[2].mv[0]), fbits(13.0f));
    /* 3 vértices con uno descartado: no alcanza */
    put8(0x90);
    put16(3);
    put8(0xFF);
    put8(1);
    put8(2);
    run();
    CHECK_EQ_U64(ngot, 0);
    teardown();
}

/* XF numTexGens o numColorChans distinto de BP genMode: no se dibuja */
static void test_prim_mismatch(void)
{
    CHECK(setup() == 0);
    xf1(0x103F, 1);
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(ngot, 0);
    bp(0x00, 1);                                        /* genMode: 1 coordenada */
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(ngot, 3);
    CHECK_EQ_U64(got_ntex, 1);
    xf1(0x1009, 2);
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(ngot, 0);
    bp(0x00, 1u | 2u << 4);
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(ngot, 3);
    teardown();
}

/* La caché: la normal del último vértice cargado, para los formatos sin normal */
static void test_prim_normal_cache(void)
{
    CHECK(setup() == 0);
    /* VAT 1: posición u8 XY y normal s8 directa; VAT 0 sin normal */
    cp(0x50, 1u << 9 | 1u << 11);
    cp(0x71, 1u << 10);                                 /* normal s8 */
    put8(0x91);
    put16(3);
    const int8_t nz[3][3] = { { 64, 0, 0 }, { 0, 0, 64 }, { 0, -64, 0 } };
    for (int k = 0; k < 3; k++) {
        put8(0);
        put8(0);
        for (int c = 0; c < 3; c++)
            put8((uint8_t)nz[k][c]);
    }
    cp(0x50, 1u << 9);
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(ngot, 6);
    CHECK_EQ_U64(fbits(got[0].nrm[0][0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(got[3].nrm[0][1]), fbits(-1.0f));    /* la del último: (0, -1, 0) */
    CHECK_EQ_U64(fbits(got[5].nrm[0][1]), fbits(-1.0f));

    /* Último vértice descartado: la caché no cambia (y con 2 cargados no se dibuja) */
    cp(0x50, 2u << 9 | 1u << 11);
    cp(0xA0, 0x1000);
    cp(0xB0, 2);
    put8(0x91);
    put16(3);
    for (int k = 0; k < 3; k++) {
        put8(k == 2 ? 0xFF : 0);
        put8(0);
        put8(0);
        put8(k == 1 ? 64 : 0);                          /* el 1: (0, 0, 1) */
    }
    cp(0x50, 1u << 9);
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(ngot, 3);
    CHECK_EQ_U64(fbits(got[0].nrm[0][1]), fbits(-1.0f));
    teardown();
}

/* Un cambio de matriz, de escala de BP o de VAT llega a la primitiva siguiente */
static void test_prim_state_changes(void)
{
    CHECK(setup() == 0);
    prim(0x90, 3);
    xf1(0x003, fbits(100.0f));                          /* traslación x */
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(ngot, 6);
    CHECK_EQ_U64(fbits(got[1].mv[0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(got[4].mv[0]), fbits(101.0f));

    /* VAT 0 a s8 XY con 1 bit de fracción y ByteDequant */
    cp(0x70, 1u << 1 | 1u << 4 | 1u << 30);
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(fbits(got[2].mv[0]), fbits(101.0f));       /* 2 / 2 + 100 */

    /* Escala de la coordenada 0: BP 0x30 (S) y 0x31 (T), y la matriz de XF A */
    cp(0x70, 0);
    xf1(0x103F, 1);
    bp(0x00, 1);
    xf1(0x1040, 0);                                     /* regular, AB11, desde la posición */
    xf1(0x1018, 5u << 6);                               /* coordenada 0 con la matriz 5 */
    const uint32_t m5[8] = { fbits(1), 0, 0, fbits(0.5f), 0, fbits(1), 0, 0 };
    xf(5 * 4, 8, m5);
    bp(0x30, 3);
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(ngot, 3);
    CHECK_EQ_U64(fbits(got[2].tex[0][0]), fbits(10.0f));    /* (2 + 0.5) × 4 */
    bp(0x30, 0);
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(fbits(got[2].tex[0][0]), fbits(2.5f));

    /* El índice de matriz por omisión es el de XF, no el de CP */
    cp(0x30, 0);
    xf1(0x1018, 1);                                     /* posición con la matriz 1 (ceros) */
    prim(0x90, 3);
    run();
    CHECK_EQ_U64(fbits(got[2].mv[0]), 0);
    teardown();
}

/* El texto del volcado y el NaN canónico */
static void test_prim_vertex_text(void)
{
    gx_vtx_out_t v;
    char text[GC_GX_VERTEX_TEXT];
    memset(&v, 0, sizeof v);
    v.mv[0] = 1.0f;
    v.proj[3] = -0.0f;
    v.nrm[0][0] = -NAN;
    v.col[0][0] = 0x12;
    v.col[1][3] = 0xAB;
    v.tex[1][2] = 2.0f;
    gc_gx_vertex_text(&v, 2, text);
    CHECK(strcmp(text, "v 3f800000 00000000 00000000 p 00000000 00000000 00000000 80000000 n "
                       "7fc00000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 "
                       "00000000 c 12000000 000000ab t2 00000000 00000000 00000000 00000000 "
                       "00000000 40000000") == 0);
    gc_gx_vertex_text(&v, 15, text);                    /* más de 8: solo 8 */
    CHECK(strstr(text, " t15 ") != NULL);
    CHECK(strlen(text) == strlen("v 3f800000 00000000 00000000 p 00000000 00000000 00000000 80000000 n") +
                              9 * 9 + 2 + 18 + 4 + 8 * 27);
}

const test_case_t prim_tests[] = {
    { "prim: vértices que usa cada primitiva", test_prim_referenced },
    { "prim: descarte por índice antes de contar", test_prim_skip },
    { "prim: XF y BP distintos no dibujan", test_prim_mismatch },
    { "prim: caché de normales", test_prim_normal_cache },
    { "prim: cambios de estado entre primitivas", test_prim_state_changes },
    { "prim: texto del volcado", test_prim_vertex_text },
    { NULL, NULL },
};
