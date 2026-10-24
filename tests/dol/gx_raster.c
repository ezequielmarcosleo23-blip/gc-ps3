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
 * DOL de prueba "gx_raster" (Fase 2, etapa C): de los vértices transformados
 * a los triángulos que llegan al rasterizador, con la mini-librería propia
 * (gxlib y gxvtx, sin libogc):
 *
 *   1. recorte contra cada plano (los cuatro lados, cerca, lejos, detrás de la
 *      cámara), contra varios a la vez, triángulos afuera del todo y
 *      degenerados, tiras, abanicos y cuadrados que cruzan los bordes;
 *   2. culling: cada modo con triángulos de los dos sentidos, también con el
 *      viewport al revés;
 *   3. líneas: anchos, pendientes, recortadas, el desplazamiento de las
 *      coordenadas de textura;
 *   4. puntos: tamaños y desplazamiento de coordenadas;
 *   5. sin detección de recorte (XF 0x1005);
 *   6. proyección ortográfica.
 *
 * Lo que se compara es cada triángulo (posición en la pantalla, w, colores y
 * coordenadas interpoladas al recortar) contra el renderizador por software de
 * Dolphin (tests/dol/dolphin/gx_raster.tri.txt).
 *
 * Solo para el DOL: no compila en la PC.
 */
#include "gxlib.h"
#include "rt.h"

#define FIFO_SIZE (64u * 1024u)
#define MAX_WAIT  2000000u

static uint8_t fifo[FIFO_SIZE] __attribute__((aligned(32)));

static uint32_t token;
static int fails;

static const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
/* Textura proyectada: q = -z */
static const float tex_proj[12] = { 0.5f, 0, 0.25f, 0, 0, -0.5f, 0.125f, 0, 0, 0, -1, 0 };
static const float persp[6] = { 1.2990381f, 0, 1.7320508f, 0, -0.001001001f, -1.001001f };
static const float ortho[6] = { 0.003125f, -1.0f, -0.0041666668f, 1.0f, -0.001f, -1.0f };
static const float viewport[6] = { 320.0f, -240.0f, 16777215.0f, 342.0f, 262.0f, 16777215.0f };
static const float viewport_up[6] = { 320.0f, 240.0f, 16777215.0f, 342.0f, 262.0f, 16777215.0f };

static void sync_gpu(const char *what)
{
    token++;
    gx_bp(BP_TOKEN, token);
    gx_flush();
    if (!gx_wait_idle(MAX_WAIT)) {
        OSReport("gx_raster: %s: la GPU no vacio el FIFO\n", what);
        fails++;
        return;
    }
    uint32_t n = 0;
    while (PE_REG(PE_TOKEN) != (uint16_t)token && n < MAX_WAIT)
        n++;
    if (n == MAX_WAIT) {
        OSReport("gx_raster: %s: el token %u no llego\n", what, (unsigned)token);
        fails++;
        return;
    }
    OSReport("gx_raster: %s\n", what);
}

/* BP genMode: coordenadas, canales y culling (0 nada, 1 atrás, 2 adelante, 3 todo) */
static void genmode(uint32_t texgens, uint32_t chans, uint32_t cull)
{
    gx_bp(BP_GENMODE, texgens | chans << 4 | cull << 14);
    gx_xf1(XF_NUMTEXGEN, texgens);
    gx_xf1(XF_NUMCHAN, chans);
}

/* Un vértice: posición, dos colores y una coordenada */
typedef struct v {
    float x, y, z;
    uint32_t c0, c1;
    float s, t;
} v_t;

static void put(const v_t *v)
{
    gx_f32(v->x);
    gx_f32(v->y);
    gx_f32(v->z);
    gx_u32(v->c0);
    gx_u32(v->c1);
    gx_f32(v->s);
    gx_f32(v->t);
}

static void prim(uint32_t op, const v_t *v, uint32_t n)
{
    gx_begin(op, 0, n);
    for (uint32_t k = 0; k < n; k++)
        put(&v[k]);
}

/* Un vértice a partir de su posición, con colores y coordenadas que dependen de ella */
static v_t at(float x, float y, float z)
{
    static uint32_t k;
    v_t v;
    v.x = x;
    v.y = y;
    v.z = z;
    k++;
    v.c0 = 0xFF000000u >> (8 * (k % 4)) | 0x10203040u * (k % 7);
    v.c1 = 0x0F1E2D3Cu * (k % 5 + 1);
    v.s = x * 0.25f + 0.5f;
    v.t = 0.5f - y * 0.125f;
    return v;
}

static void tri(v_t a, v_t b, v_t c)
{
    const v_t v[3] = { a, b, c };
    prim(GX_TRIANGLES, v, 3);
}

/* ---- 0. estado inicial -------------------------------------------------------------- */

static void setup(void)
{
    gx_init_fifo(fifo, FIFO_SIZE, FIFO_SIZE - 16384, FIFO_SIZE / 2, CP_CR_READ | CP_CR_LINK);
    genmode(2, 2, 0);
    gx_xf1(XF_DUALTEX, 0);
    gx_projection(persp, 0);
    gx_xf_floats(XF_VIEWPORT, 6, viewport);
    gx_pos_matrix(0, ident);
    gx_pos_matrix(30, ident);
    gx_pos_matrix(33, tex_proj);
    gx_matindex(0 | 30u << 6 | 33u << 12, 0);
    for (int ch = 0; ch < 2; ch++) {
        gx_xf1(XF_COLOR0 + ch, CH_MATSRC_VTX);
        gx_xf1(XF_ALPHA0 + ch, CH_MATSRC_VTX);
    }
    gx_xf1(XF_TEXMTXINFO + 0, TG_ROW(5));                     /* Tex0, ST */
    gx_xf1(XF_TEXMTXINFO + 1, TG_STQ | TG_ABC1 | TG_ROW(0));  /* posición, STQ */
    gx_bp(BP_SU_SSIZE, 127);
    gx_bp(BP_SU_TSIZE, 63);
    gx_bp(BP_SU_SSIZE + 2, 255);
    gx_bp(BP_SU_TSIZE + 2, 31);
    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_8888) | VAT_COL1(COL_8888) | VAT_TEX0(1, FMT_F32, 0),
           0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT) | VCD_COL1(VTX_DIRECT), VCD_TEX(0, VTX_DIRECT),
               0);
    sync_gpu("estado inicial");
}

/* ---- 1. recorte ---------------------------------------------------------------------- */

static void clipping(void)
{
    /* Adentro */
    tri(at(-1, -1, -5), at(1, -1, -5), at(0, 1, -5));
    /* Cada lado: x' = 1,3 x contra w = 5 */
    tri(at(-1, -1, -5), at(6, 0, -5), at(0, 1, -5));
    tri(at(-6, 0.5f, -5), at(1, -1, -5), at(0, 1, -5));
    tri(at(-1, -1, -5), at(1, -1, -5), at(0, 4, -5));
    tri(at(-1, 1, -5), at(0, -4, -5), at(1, 1, -5));
    /* Cerca (detrás del plano cercano, en z = -1) y detrás de la cámara */
    tri(at(-1, -1, -5), at(1, -1, -5), at(0, 0.5f, -0.5f));
    tri(at(-1, -1, -5), at(0.5f, 0, 2), at(0, 1, -5));
    /* Lejos (más allá de 1000) */
    tri(at(-1, -1, -5), at(1, -1, -5), at(0, 100, -1500));
    /* Dos y más planos a la vez */
    tri(at(-1, -1, -5), at(8, 8, -5), at(0, 1, -5));
    tri(at(-50, -50, -3), at(50, -50, -3), at(0, 80, -3));
    tri(at(-40, -40, 3), at(60, -10, -2000), at(-5, 70, -0.25f));
    /* Afuera del todo y degenerados */
    tri(at(10, 0, -5), at(12, 1, -5), at(11, 2, -5));
    tri(at(-1, -1, -5), at(0, 0, -5), at(1, 1, -5));
    tri(at(0.5f, 0.5f, -5), at(0.5f, 0.5f, -5), at(0.5f, 0.5f, -5));

    /* Tira, abanico y cuadrados que cruzan los bordes */
    const v_t strip[7] = { at(-6, -1, -5), at(-5, 1, -4), at(-2, -1, -3), at(0, 1, -0.5f),
                           at(2, -1, -6), at(5, 1, -5), at(7, -1, -5) };
    prim(GX_TRIANGLE_STRIP, strip, 7);
    const v_t fan[6] = { at(0, 0, -5), at(4, 0, -5), at(3, 3, -5), at(0, 6, -5), at(-3, 3, -0.2f),
                         at(-6, 0, -5) };
    prim(GX_TRIANGLE_FAN, fan, 6);
    const v_t quads[7] = { at(-1, -1, -5), at(1, -1, -5), at(1, 1, -5), at(-1, 1, -5),
                           at(3, 3, -5), at(9, 3, -5), at(9, 9, -5) };
    prim(GX_QUADS, quads, 7);
    sync_gpu("1. recorte");
}

/* ---- 2. culling ---------------------------------------------------------------------- */

static void culling(void)
{
    for (uint32_t pass = 0; pass < 2; pass++) {
        gx_xf_floats(XF_VIEWPORT, 6, pass ? viewport_up : viewport);
        for (uint32_t cull = 0; cull < 4; cull++) {
            genmode(2, 2, cull);
            tri(at(-1, -1, -4), at(1, -1, -4), at(0, 1, -4));       /* antihorario */
            tri(at(-1, -1, -4), at(0, 1, -4), at(1, -1, -4));       /* horario */
            /* Con un vértice afuera: el que se descarta igual deja la pendiente de Z */
            tri(at(-1, -1, -4), at(6, 0, -4), at(0, 1, -4));
        }
    }
    gx_xf_floats(XF_VIEWPORT, 6, viewport);
    genmode(2, 2, 0);
    sync_gpu("2. culling");
}

/* ---- 3. líneas --------------------------------------------------------------------- */

static void lines(void)
{
    static const uint32_t width[4] = { 6, 12, 30, 0 };
    for (uint32_t k = 0; k < 4; k++) {
        /* Ancho, y el desplazamiento de coordenadas k + 1 (1/16 ... 1/2) */
        gx_bp(0x22, width[k] | (k + 1) << 16);
        gx_bp(BP_SU_SSIZE, 127 | (k & 1u) << 18);
        const v_t l[10] = {
            at(-2, 0, -5), at(2, 0.5f, -5),                  /* casi horizontal */
            at(0, -2, -5), at(0.3f, 2, -5),                  /* casi vertical */
            at(-1, -1, -5), at(1, 1, -5),                    /* a 45 grados */
            at(-1, 0, -5), at(9, 1, -5),                     /* recortada a la derecha */
            at(0, 0, -5), at(0.5f, 0.5f, 3),                 /* detrás de la cámara */
        };
        prim(GX_LINES, l, 10);
        const v_t s[4] = { at(-3, -2, -6), at(-1, 2, -6), at(1, -2, -0.4f), at(3, 2, -6) };
        prim(GX_LINE_STRIP, s, 4);
        const v_t out[2] = { at(10, 10, -5), at(12, 11, -5) };   /* afuera del todo */
        prim(GX_LINES, out, 2);
    }
    gx_bp(BP_SU_SSIZE, 127);
    sync_gpu("3. lineas");
}

/* ---- 4. puntos --------------------------------------------------------------------- */

static void points(void)
{
    static const uint32_t size[3] = { 6, 18, 0 };
    for (uint32_t k = 0; k < 3; k++) {
        gx_bp(0x22, size[k] << 8 | (k + 3) << 19);
        gx_bp(BP_SU_SSIZE, 127 | 1u << 19);
        gx_bp(BP_SU_SSIZE + 2, 255 | (k & 1u) << 19);
        const v_t p[5] = { at(0, 0, -5), at(1.5f, -1, -5), at(20, 0, -5), at(0.5f, 0.5f, 2),
                           at(-1, 1, -0.5f) };
        prim(GX_POINTS, p, 5);
    }
    gx_bp(0x22, 0);
    gx_bp(BP_SU_SSIZE, 127);
    gx_bp(BP_SU_SSIZE + 2, 255);
    sync_gpu("4. puntos");
}

/* ---- 5. sin detección de recorte ---------------------------------------------------- */

/* XF 0x1005 no vacía el lote de Dolphin: el token tiene que llegar antes de
 * volver a 0, si no Dolphin dibuja el lote con el recorte otra vez */
static void no_clipping(void)
{
    gx_xf1(0x1005, 1);
    tri(at(-1, -1, -5), at(6, 0, -5), at(0, 4, -5));             /* w > 0: no se recorta */
    tri(at(-1, -1, -5), at(1, -1, -5), at(0, 0.5f, -0.5f));
    tri(at(-1, -1, -5), at(0.5f, 0, 2), at(0, 1, -5));           /* w < 0: se recorta */
    sync_gpu("5. sin deteccion de recorte");
    gx_xf1(0x1005, 0);
}

/* ---- 6. ortográfica ------------------------------------------------------------------ */

static void orthographic(void)
{
    gx_projection(ortho, 1);
    tri(at(100, 100, -0.5f), at(500, 120, -0.5f), at(300, 400, -0.5f));
    tri(at(-200, 100, -10), at(300, -50, -10), at(200, 300, -10));   /* sale por la izquierda */
    tri(at(100, 100, 0.5f), at(300, 120, -500), at(200, 300, -1500)); /* cerca y lejos */
    gx_projection(persp, 0);
    sync_gpu("6. proyeccion ortografica");
}

int main(void)
{
    OSReport("gx_raster: triangulos, recorte, lineas y puntos\n");
    setup();
    clipping();
    culling();
    lines();
    points();
    no_clipping();
    orthographic();
    OSReport("gx_raster: %u tokens; %s\n", (unsigned)token, fails ? "FALLO" : "OK");
    exit(fails ? 1 : 0);
}
