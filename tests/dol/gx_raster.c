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
 *   6. proyección ortográfica;
 *
 * y del triángulo al píxel (paso C3), con una proyección ortográfica en
 * píxeles exactos:
 *
 *   7. relleno: Gouraud, la convención de relleno con bordes en fracciones de
 *      píxel, triángulos finos y diminutos, abanicos, líneas y puntos;
 *   8. tijera: rectángulos impares, el desplazamiento y la vuelta por 1024
 *      que da dos rectángulos;
 *   9. Z: cada comparación, sin escritura, temprana y tardía con la prueba de
 *      alfa, zfreeze, recorte de Z y la prueba apagada;
 *  10. mezcla: los 64 pares de factores, resta, las 16 operaciones lógicas,
 *      alfa de destino, máscaras y dither, en RGB8 y RGBA6;
 *  11. formatos de píxel, prueba de alfa, registros y constantes del TEV;
 *  12. el CPU lee el EFB (0xC8000000) con los modos de alfa del PE, y la caja
 *      envolvente.
 *
 * Lo que se compara es cada triángulo (posición en la pantalla, w, colores y
 * coordenadas interpoladas al recortar) contra el renderizador por software de
 * Dolphin (tests/dol/dolphin/gx_raster.tri.txt), y desde la parte 7 el EFB:
 * cada parte termina con copias sin destino (paso 0) que anotan el CRC de lo
 * dibujado (gx_raster.cop.txt), y las lecturas del CPU van en la huella.
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


/* ---- 7-12: del triángulo al píxel (paso C3) ------------------------------------------ */

/* Ortográfica en píxeles exactos: x, y del EFB y z de 24 bits, sin redondeo
 * (escalas de potencias de 2): x de la pantalla = x + 342, igual y; z = z */
static const float ortho_px[6] = { 1.0f / 512.0f, -1.0f, -1.0f / 512.0f, 1.0f, 1.0f / 16777216.0f, -1.0f };
static const float viewport_px[6] = { 512.0f, -512.0f, 16777216.0f, 854.0f, 854.0f, 16777216.0f };

enum {
    BP_SCISSOR_TL = 0x20, BP_SCISSOR_BR = 0x21, BP_LINEPT = 0x22, BP_TEVORDER = 0x28,
    BP_ZMODE = 0x40, BP_BLEND = 0x41, BP_DSTALPHA = 0x42, BP_ZCONTROL = 0x43,
    BP_COPY_XY = 0x49, BP_COPY_WH = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D,
    BP_CLEAR_AR = 0x4F, BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51, BP_COPY = 0x52, BP_BBOX0 = 0x55,
    BP_BBOX1 = 0x56, BP_SCISSOR_OFFSET = 0x59, BP_COLOR_C0 = 0xC0, BP_ALPHA_C0 = 0xC1,
    BP_TEV_RA = 0xE0, BP_TEV_BG = 0xE1, BP_ALPHATEST = 0xF3, BP_KSEL = 0xF6,
};
enum { PF_RGB8 = 0, PF_RGBA6 = 1, PF_RGB565 = 2, PF_Z24 = 3 };
enum { Z_NEVER = 0, Z_LESS = 1, Z_EQUAL = 2, Z_LEQUAL = 3, Z_GREATER = 4, Z_NEQUAL = 5, Z_GEQUAL = 6,
       Z_ALWAYS = 7 };
#define ZMODE(func, update) (1u | (uint32_t)(func) << 1 | (uint32_t)(update) << 4)
#define B_ENABLE   1u
#define B_LOGIC    2u
#define B_DITHER   4u
#define B_COLOR    8u
#define B_ALPHA    16u
#define B_DST(f)   ((uint32_t)(f) << 5)
#define B_SRC(f)   ((uint32_t)(f) << 8)
#define B_SUB      (1u << 11)
#define B_OP(o)    ((uint32_t)(o) << 12)
/* Combinadores: d, c, b, a, y la salida con recorte a 0-255 */
#define CC(a, b, c, d) ((uint32_t)(a) << 12 | (uint32_t)(b) << 8 | (uint32_t)(c) << 4 | (uint32_t)(d) | 1u << 19)
#define AC(a, b, c, d) ((uint32_t)(a) << 13 | (uint32_t)(b) << 10 | (uint32_t)(c) << 7 | (uint32_t)(d) << 4 | 1u << 19)
#define ALWAYS_PASS (7u << 16 | 7u << 19)

static uint32_t peeks;                          /* suma de las lecturas del EFB */

static void pv(float x, float y, float z, uint32_t c)
{
    gx_f32(x);
    gx_f32(y);
    gx_f32(z);
    gx_u32(c);
    gx_u32(c);
    gx_f32(0);
    gx_f32(0);
}

static void ptri(float x0, float y0, float x1, float y1, float x2, float y2, float z, uint32_t c0,
                 uint32_t c1, uint32_t c2)
{
    gx_begin(GX_TRIANGLES, 0, 3);
    pv(x0, y0, z, c0);
    pv(x1, y1, z, c1);
    pv(x2, y2, z, c2);
}

/* Rectángulo de dos triángulos (cuadrado de GX) con un color por esquina */
static void rect4(float x, float y, float w, float h, float z, uint32_t c0, uint32_t c1, uint32_t c2,
                  uint32_t c3)
{
    gx_begin(GX_QUADS, 0, 4);
    pv(x, y, z, c0);
    pv(x + w, y, z, c1);
    pv(x + w, y + h, z, c2);
    pv(x, y + h, z, c3);
}

static void rect(float x, float y, float w, float h, float z, uint32_t c)
{
    rect4(x, y, w, h, z, c, c, c, c);
}

/* Copia sin destino (paso 0): no escribe la RAM, Dolphin y el runner anotan
 * el CRC del rectángulo del EFB; con clear, además lo borra */
static void copy_efb(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t trigger)
{
    gx_bp(BP_COPY_XY, x | y << 10);
    gx_bp(BP_COPY_WH, (w - 1) | (h - 1) << 10);
    gx_bp(BP_COPY_DST, 0);
    gx_bp(BP_COPY_STRIDE, 0);
    gx_bp(BP_COPY, trigger);
}

static void snap(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    copy_efb(x, y, w, h, 0);
}

/* Borra todo el EFB con el color 0xRRGGBBAA y z (según las máscaras de ahora) */
static void clear_efb(uint32_t rgba, uint32_t z)
{
    gx_bp(BP_CLEAR_AR, (rgba & 0xFFu) << 8 | rgba >> 24);
    gx_bp(BP_CLEAR_GB, (rgba >> 8) & 0xFFFFu);
    gx_bp(BP_CLEAR_Z, z);
    copy_efb(0, 0, 640, 528, 1u << 11);
}

static void pixel_state(uint32_t pf, uint32_t zmode, uint32_t blend)
{
    gx_bp(BP_ZCONTROL, pf);
    gx_bp(BP_ZMODE, zmode);
    gx_bp(BP_BLEND, blend);
}

static void scissor(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    gx_bp(BP_SCISSOR_TL, (y0 + 342) | (x0 + 342) << 12);
    gx_bp(BP_SCISSOR_BR, (y1 + 342) | (x1 + 342) << 12);
}

/* Una etapa de TEV que pasa el color del rasterizador, tabla de swap 0
 * normal, sin prueba de alfa, la tijera en todo el EFB y RGB8 con Z */
static void pixel_setup(void)
{
    gx_projection(ortho_px, 1);
    gx_xf_floats(XF_VIEWPORT, 6, viewport_px);
    genmode(0, 1, 0);
    scissor(0, 0, 639, 527);
    gx_bp(BP_SCISSOR_OFFSET, 171 | 171u << 10);
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 10));     /* PREV = ras.rgb */
    gx_bp(BP_ALPHA_C0, AC(7, 7, 7, 5));         /* PREV = ras.a */
    gx_bp(BP_TEVORDER, 0);                      /* COLOR0, sin textura */
    gx_bp(BP_KSEL, 1u << 2);                    /* swap 0: r g b a */
    gx_bp(BP_KSEL + 1, 2u | 3u << 2);
    gx_bp(BP_ALPHATEST, ALWAYS_PASS);
    pixel_state(PF_RGB8, ZMODE(Z_LEQUAL, 1), B_COLOR | B_ALPHA);
}

/* ---- 7. relleno ------------------------------------------------------------------------ */

static void fill(void)
{
    pixel_setup();
    clear_efb(0x00000000u, 0xFFFFFFu);

    /* Gouraud grande, con z inclinado */
    gx_begin(GX_TRIANGLES, 0, 3);
    pv(20, 20, 1000, 0xFF0000FFu);
    pv(300, 40, 9000000, 0x00FF00FFu);
    pv(100, 250, 16000000, 0x0000FFFFu);
    snap(0, 0, 320, 260);

    /* Sumando (ONE + ONE): un píxel que se dibuja dos veces se nota */
    pixel_state(PF_RGB8, 0, B_ENABLE | B_DST(1) | B_SRC(1) | B_COLOR | B_ALPHA);
    for (uint32_t i = 0; i < 16; i++) {
        const float o = (float)i / 16.0f;
        rect(330 + 18 * (float)(i % 8) + o, 20 + 18 * (float)(i / 8) + o, 10, 10, 0, 0x40404040u);
        /* Bordes en 0,5 exacto y lados compartidos por dos triángulos */
        ptri(330 + 18 * (float)(i % 8) + 0.5f, 70 + 18 * (float)(i / 8), 340 + 18 * (float)(i % 8) + o,
             70 + 18 * (float)(i / 8) + o, 330 + 18 * (float)(i % 8) + 0.5f, 80 + 18 * (float)(i / 8),
             0, 0x30303030u, 0x30303030u, 0x30303030u);
    }
    /* Bordes justo sobre las muestras (k + 9/16): cada arista primero en una
     * de las tres rotaciones, en los dos sentidos, horizontales, verticales y
     * a 45 grados */
    for (uint32_t k = 0; k < 6; k++) {
        static const float t[2][3][2] = {
            { { 0, 0 }, { 8, 0 }, { 0, 8 } },               /* horizontal y vertical */
            { { 0, 0 }, { 8, 8 }, { 8, 0 } },               /* a 45 grados */
        };
        const uint32_t shape = k / 3, rot = k % 3;
        for (uint32_t dir = 0; dir < 2; dir++) {
            const float bx = 330.5625f + 12 * (float)k, by = 110.5625f + 12 * (float)dir;
            gx_begin(GX_TRIANGLES, 0, 3);
            for (uint32_t n = 0; n < 3; n++) {
                const uint32_t i = (rot + (dir ? 3 - n : n)) % 3;
                pv(bx + t[shape][i][0], by + t[shape][i][1], 0, 0x50505050u);
            }
        }
    }
    /* Abanico de 24 triángulos finos alrededor de un centro fraccionario */
    gx_begin(GX_TRIANGLE_FAN, 0, 26);
    pv(480.3f, 160.6f, 0, 0x20202020u);
    for (uint32_t k = 0; k <= 24; k++) {
        static const float c[25][2] = {
            { 60, 0 }, { 58, 15.5f }, { 52, 30 }, { 42.4f, 42.4f }, { 30, 52 }, { 15.5f, 58 }, { 0, 60 },
            { -15.5f, 58 }, { -30, 52 }, { -42.4f, 42.4f }, { -52, 30 }, { -58, 15.5f }, { -60, 0 },
            { -58, -15.5f }, { -52, -30 }, { -42.4f, -42.4f }, { -30, -52 }, { -15.5f, -58 }, { 0, -60 },
            { 15.5f, -58 }, { 30, -52 }, { 42.4f, -42.4f }, { 52, -30 }, { 58, -15.5f }, { 60, 0 },
        };
        pv(480.3f + c[k][0], 160.6f + c[k][1], 0, 0x20202020u);
    }
    /* Diminutos (dentro de un píxel, o tocando un centro) y astillas */
    ptri(20.1f, 300.1f, 20.9f, 300.2f, 20.5f, 300.9f, 0, 0x80808080u, 0x80808080u, 0x80808080u);
    ptri(24.4f, 300.4f, 24.6f, 300.4f, 24.5f, 300.6f, 0, 0x80808080u, 0x80808080u, 0x80808080u);
    ptri(28.5f, 300.5f, 29.5f, 300.5f, 28.5f, 301.5f, 0, 0x80808080u, 0x80808080u, 0x80808080u);
    ptri(40, 300, 300, 301, 40, 300.3f, 0, 0x80808080u, 0x80808080u, 0x80808080u);
    ptri(40, 310, 41, 310, 300, 420, 0, 0x60606060u, 0x60606060u, 0x60606060u);
    snap(0, 0, 640, 528);

    /* Líneas y puntos de 1, 2 y 3 píxeles */
    pixel_state(PF_RGB8, ZMODE(Z_LEQUAL, 1), B_COLOR | B_ALPHA);
    for (uint32_t k = 0; k < 3; k++) {
        gx_bp(BP_LINEPT, 6 * (k + 1) | (6 * (k + 1)) << 8);   /* en sextos de píxel */
        gx_begin(GX_LINES, 0, 6);
        pv(330, 300 + 40 * (float)k, 100, 0xFFFF00FFu);
        pv(620, 320 + 40 * (float)k, 100, 0x00FFFFFFu);
        pv(340.5f, 290 + 40 * (float)k, 100, 0xFF00FFFFu);
        pv(345.25f, 330.75f + 40 * (float)k, 100, 0xFFFFFFFFu);
        pv(400, 300.5f + 40 * (float)k, 100, 0x808080FFu);
        pv(600, 300.5f + 40 * (float)k, 100, 0x808080FFu);
        gx_begin(GX_POINTS, 0, 3);
        pv(500.5f, 450 + 20 * (float)k, 100, 0xFF8000FFu);
        pv(520.25f, 450.75f + 20 * (float)k, 100, 0x0080FFFFu);
        pv(540, 450 + 20 * (float)k, 100, 0x80FF00FFu);
    }
    gx_bp(BP_LINEPT, 0);
    snap(0, 0, 640, 528);
    sync_gpu("7. relleno");
}

/* ---- 8. tijera ------------------------------------------------------------------------- */

static void scissors(void)
{
    pixel_setup();
    pixel_state(PF_RGB8, 0, B_COLOR | B_ALPHA);
    clear_efb(0x102030FFu, 0);

    /* Impar: [13, 100] × [7, 51] */
    scissor(13, 7, 100, 51);
    rect4(0, 0, 200, 100, 0, 0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFFFFFFu);
    /* Una columna de un píxel y una fila */
    scissor(150, 0, 150, 99);
    rect(110, 0, 100, 100, 0, 0xFFFF00FFu);
    scissor(110, 60, 209, 60);
    rect(110, 0, 100, 100, 0, 0x00FFFFFFu);
    /* Al revés (izquierda > derecha): nada */
    scissor(300, 0, 250, 99);
    rect(220, 0, 100, 100, 0, 0xFFFFFFFFu);
    snap(0, 0, 320, 110);

    /* Desplazamiento: la tijera en (40, 120)-(239, 239) y el origen corrido 20 y 10 */
    scissor(40, 120, 239, 239);
    gx_bp(BP_SCISSOR_OFFSET, (171 + 10) | (171u + 5) << 10);
    rect4(0, 100, 300, 160, 0, 0xFF8040FFu, 0x40FF80FFu, 0x8040FFFFu, 0x808080FFu);
    gx_bp(BP_SCISSOR_OFFSET, 171 | 171u << 10);
    snap(0, 100, 320, 160);

    /* Vuelta por 1024: tijera de 0 a 2047 en x sin desplazamiento: dos
     * rectángulos, el triángulo sale dos veces (x de la pantalla 342 + x) */
    gx_bp(BP_SCISSOR_TL, 342u | 0u << 12);
    gx_bp(BP_SCISSOR_BR, (342u + 527) | 2047u << 12);
    gx_bp(BP_SCISSOR_OFFSET, 0 | 171u << 10);
    ptri(300, 300, 1000, 330, 650, 520, 0, 0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu);
    rect(720, 400, 60, 60, 0, 0xFFFFFFFFu);    /* solo en el de 1024: x de 38 a 98 del EFB */
    snap(0, 280, 640, 248);
    /* Con un viewport de 2048 de ancho el mismo triángulo cae en los dos
     * rectángulos y se pisa: el mejor (el de 1024, más adentro del viewport)
     * va último */
    static const float ortho_wide[6] = { 1.0f / 1024.0f, -1.0f, -1.0f / 512.0f, 1.0f, 1.0f / 16777216.0f, -1.0f };
    static const float vp_wide[6] = { 1024.0f, -512.0f, 16777216.0f, 1366.0f, 854.0f, 16777216.0f };
    gx_projection(ortho_wide, 1);
    gx_xf_floats(XF_VIEWPORT, 6, vp_wide);
    clear_efb(0x000000FFu, 0);
    rect4(0, 300, 1600, 200, 0, 0xFF0000FFu, 0x0000FFFFu, 0x00FF00FFu, 0xFFFFFFFFu);
    snap(0, 280, 640, 248);
    pixel_setup();
    sync_gpu("8. tijera");
}

/* ---- 9. Z ------------------------------------------------------------------------------ */

static void depth(void)
{
    pixel_setup();
    pixel_state(PF_RGB8, ZMODE(Z_ALWAYS, 1), B_COLOR | B_ALPHA);
    clear_efb(0x000000FFu, 0x800000u);

    /* Cada comparación contra 0x800000, con Z de 0x400000 a 0xC00000 en el triángulo */
    for (uint32_t f = 0; f < 8; f++) {
        pixel_state(PF_RGB8, ZMODE(f, 1), B_COLOR | B_ALPHA);
        const float x = 10 + 78 * (float)f;
        gx_begin(GX_TRIANGLES, 0, 3);
        pv(x, 10, 0x400000, 0xFF0000FFu);
        pv(x + 70, 10, 0x800000, 0x00FF00FFu);
        pv(x, 80, 0xC00000, 0x0000FFFFu);
        /* Y uno plano en 0x800000 justo: igual, menor o igual... */
        rect(x + 40, 40, 30, 30, 0x800000, 0xFFFFFFFFu);
    }
    snap(0, 0, 640, 90);

    /* Sin escritura de Z, y la prueba apagada (tampoco escribe) */
    pixel_state(PF_RGB8, ZMODE(Z_LESS, 0), B_COLOR | B_ALPHA);
    rect4(10, 100, 100, 60, 0x100000, 0xFF00FFFFu, 0xFF00FFFFu, 0x00FF00FFu, 0x00FF00FFu);
    rect(60, 120, 100, 60, 0x700000, 0xFFFF00FFu);
    pixel_state(PF_RGB8, 0 | 1u << 4, B_COLOR | B_ALPHA);
    rect(180, 100, 100, 60, 0x100000, 0x00FFFFFFu);
    pixel_state(PF_RGB8, ZMODE(Z_LESS, 1), B_COLOR | B_ALPHA);
    rect(230, 130, 100, 60, 0x700000, 0xFF8000FFu);
    snap(0, 90, 640, 100);

    /* Temprana y tardía con la prueba de alfa que falla: la temprana igual
     * escribe Z, la tardía no */
    gx_bp(BP_ALPHATEST, 4u << 16 | 7u << 19 | 0x80u);      /* a > 0x80 y siempre */
    for (uint32_t early = 0; early < 2; early++) {
        gx_bp(BP_ZCONTROL, PF_RGB8 | early << 6);
        const float x = 10 + 320 * (float)early;
        rect4(x, 200, 150, 80, 0x200000, 0xFF000000u, 0xFF0000FFu, 0xFF0000FFu, 0xFF000000u);
        gx_bp(BP_ALPHATEST, ALWAYS_PASS);
        rect(x + 50, 220, 150, 80, 0x300000, 0x00FF00FFu);   /* lo tapa donde quedó Z */
        gx_bp(BP_ALPHATEST, 4u << 16 | 7u << 19 | 0x80u);
    }
    gx_bp(BP_ALPHATEST, ALWAYS_PASS);
    gx_bp(BP_ZCONTROL, PF_RGB8);
    snap(0, 190, 640, 120);

    /* zfreeze: el último triángulo antes deja la pendiente y los siguientes la
     * usan; también uno descartado por culling la deja */
    pixel_state(PF_RGB8, ZMODE(Z_ALWAYS, 1), B_COLOR | B_ALPHA);
    gx_begin(GX_TRIANGLES, 0, 3);
    pv(10, 320, 0x100000, 0xFFFFFFFFu);
    pv(200, 320, 0x900000, 0xFFFFFFFFu);
    pv(10, 420, 0x500000, 0xFFFFFFFFu);
    gx_bp(BP_GENMODE, 1u << 4 | 1u << 19);
    rect(220, 320, 120, 90, 0x000000, 0x80FF80FFu);
    gx_bp(BP_GENMODE, 1u << 4 | 1u << 14);      /* culling: uno se dibuja, el otro no */
    ptri(400, 320, 500, 320, 450, 400, 0x200000, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    ptri(400, 330, 450, 410, 500, 330, 0xF00000, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu);
    gx_bp(BP_GENMODE, 1u << 4 | 1u << 19);
    rect(360, 330, 200, 80, 0x000000, 0xFF80FFFFu);
    gx_bp(BP_GENMODE, 1u << 4);
    snap(0, 310, 640, 110);

    /* Z fuera de 0-16777215: el viewport lejos lo pasa y se recorta */
    static const float vp_far[6] = { 512.0f, -512.0f, 33554432.0f, 854.0f, 854.0f, 25000000.0f };
    gx_xf_floats(XF_VIEWPORT, 6, vp_far);
    gx_begin(GX_TRIANGLES, 0, 3);
    pv(10, 430, 0, 0xFFFFFFFFu);
    pv(400, 430, 16777216, 0xFF0000FFu);
    pv(10, 520, 8000000, 0x0000FFFFu);
    gx_xf_floats(XF_VIEWPORT, 6, viewport_px);
    snap(0, 420, 640, 108);
    sync_gpu("9. Z");
}

/* ---- 10. mezcla ------------------------------------------------------------------------ */

static void blending(void)
{
    pixel_setup();
    for (uint32_t pf = 0; pf < 2; pf++) {
        pixel_state(pf, 0, B_COLOR | B_ALPHA);
        clear_efb(0x00000000u, 0);
        /* Fondo: rojo según x, verde según y, alfa según x */
        rect4(0, 0, 640, 528, 0, 0x00000000u, 0xFF0000FFu, 0xFFFF00FFu, 0x00FF0000u);

        for (uint32_t s = 0; s < 8; s++)
            for (uint32_t d = 0; d < 8; d++) {
                pixel_state(pf, 0, B_ENABLE | B_SRC(s) | B_DST(d) | B_COLOR | B_ALPHA);
                rect4(4 + 30 * (float)s, 4 + 30 * (float)d, 26, 26, 0, 0x2080C010u, 0xC0802090u,
                      0x80C020F0u, 0x40404040u);
            }
        /* Resta */
        pixel_state(pf, 0, B_ENABLE | B_SUB | B_SRC(4) | B_DST(5) | B_COLOR | B_ALPHA);
        rect4(250, 4, 60, 60, 0, 0x404040FFu, 0x80808080u, 0xFF000040u, 0x00FF0000u);
        /* Las 16 operaciones lógicas */
        for (uint32_t op = 0; op < 16; op++) {
            pixel_state(pf, 0, B_LOGIC | B_OP(op) | B_COLOR | B_ALPHA);
            rect4(250 + 24 * (float)(op % 8), 70 + 24 * (float)(op / 8), 20, 20, 0, 0x5AA5C33Cu, 0x0FF0F00Fu,
                  0x12345678u, 0x9ABCDEF0u);
        }
        /* El blend gana sobre la operación lógica */
        pixel_state(pf, 0, B_ENABLE | B_LOGIC | B_OP(12) | B_SRC(1) | B_DST(1) | B_COLOR | B_ALPHA);
        rect(250, 120, 40, 20, 0, 0x30303030u);
        /* Alfa de destino */
        gx_bp(BP_DSTALPHA, 0x100 | 0x5A);
        pixel_state(pf, 0, B_COLOR | B_ALPHA);
        rect(300, 120, 40, 20, 0, 0xC0C0C0C0u);
        gx_bp(BP_DSTALPHA, 0x5A);
        /* Máscaras: solo color, solo alfa, nada */
        pixel_state(pf, 0, B_COLOR);
        rect(250, 150, 30, 30, 0, 0x11223344u);
        pixel_state(pf, 0, B_ALPHA);
        rect(290, 150, 30, 30, 0, 0x55667788u);
        pixel_state(pf, 0, 0);
        rect(330, 150, 30, 30, 0, 0x99AABBCCu);
        /* Dither (solo cambia en RGBA6) */
        pixel_state(pf, 0, B_DITHER | B_COLOR | B_ALPHA);
        rect4(250, 190, 200, 60, 0, 0x000000FFu, 0xFF0000FFu, 0xFFFFFFFFu, 0x0000FF80u);
        pixel_state(pf, 0, B_ENABLE | B_DITHER | B_SRC(4) | B_DST(5) | B_COLOR | B_ALPHA);
        rect4(250, 260, 200, 60, 0, 0xFF000080u, 0x00FF0040u, 0x0000FFC0u, 0xFFFFFF20u);
        snap(0, 0, 640, 528);
    }
    pixel_setup();
    sync_gpu("10. mezcla");
}

/* ---- 11. formatos, prueba de alfa y TEV ------------------------------------------------- */

static void formats_alpha_tev(void)
{
    pixel_setup();
    /* Cada formato, también los que no tienen color (4-7: no escribe nada) */
    for (uint32_t pf = 0; pf < 8; pf++) {
        pixel_state(pf, ZMODE(Z_LEQUAL, 1), B_COLOR | B_ALPHA);
        clear_efb(0x336699CCu, 0xABCDEFu);
        rect4(10, 10, 100, 100, 0x123456, 0xFF0000FFu, 0x00FF0080u, 0x0000FF40u, 0xFFFFFF00u);
        snap(0, 0, 120, 120);
    }
    /* Cambio de formato sin borrar: los bits quedan y se leen con el nuevo */
    pixel_state(PF_RGBA6, ZMODE(Z_LEQUAL, 1), B_COLOR | B_ALPHA);
    clear_efb(0x00000000u, 0xFFFFFFu);
    rect4(10, 10, 100, 100, 0x123456, 0xFF0000FFu, 0x00FF0080u, 0x0000FF40u, 0xFFFFFF00u);
    pixel_state(PF_RGB8, ZMODE(Z_LEQUAL, 1), B_ENABLE | B_SRC(6) | B_DST(1) | B_COLOR);
    rect(50, 50, 100, 100, 0x100000, 0x80808080u);
    snap(0, 0, 160, 160);

    /* Prueba de alfa: alfa de 0 a 255 en x; cada comparación con 0x80 y cada lógica */
    pixel_state(PF_RGB8, 0, B_COLOR | B_ALPHA);
    clear_efb(0x000000FFu, 0);
    for (uint32_t c = 0; c < 8; c++)
        for (uint32_t l = 0; l < 4; l++) {
            gx_bp(BP_ALPHATEST, 0x80u | 0x40u << 8 | c << 16 | ((c + 3) & 7u) << 19 | l << 22);
            rect4(4 + 64 * (float)(c % 8), 4 + 30 * (float)l, 60, 26, 0, 0xFFFFFF00u, 0xFFFFFFFFu, 0xFFFFFFFFu,
                  0xFFFFFF00u);
        }
    gx_bp(BP_ALPHATEST, ALWAYS_PASS);
    snap(0, 0, 640, 130);

    /* TEV: registros C0 (color) y K0 (constante) por BP 0xE2/0xE3 y 0xE0/0xE1
     * con el bit de tipo; PREV = C0 × (1 - ras) + K0 × ras, y la constante 3/4
     * en alfa */
    gx_bp(BP_TEV_RA + 2, 0x0F0 | 0x0C0u << 12);        /* C0: r 0xF0, a 0xC0 */
    gx_bp(BP_TEV_BG + 2, 0x7FF | 0x020u << 12);        /* C0: b -1, g 0x20 */
    gx_bp(BP_TEV_RA, 0x040 | 0x0FFu << 12 | 1u << 23); /* K0: r 0x40, a 0xFF */
    gx_bp(BP_TEV_BG, 0x0E0 | 0x090u << 12 | 1u << 23); /* K0: b 0xE0, g 0x90 */
    gx_bp(BP_KSEL, 1u << 2 | 12u << 4 | 2u << 9);      /* swap 0, kcsel K0, kasel 3/4 */
    gx_bp(BP_COLOR_C0, CC(2, 14, 10, 15));
    gx_bp(BP_ALPHA_C0, AC(1, 6, 5, 7));
    rect4(10, 140, 300, 100, 0, 0x000000FFu, 0xFF000080u, 0x00FF00FFu, 0xFFFFFF00u);
    /* Sin recorte (C0 con -1 en azul), escala 2 y resta con sesgo */
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 2) & ~(1u << 19));
    gx_bp(BP_ALPHA_C0, AC(7, 7, 7, 1) | 1u << 20);
    rect(320, 140, 100, 100, 0, 0xFFFFFFFFu);
    gx_bp(BP_COLOR_C0, CC(10, 14, 13, 2) | 1u << 18 | 1u << 16 | 1u << 20);
    gx_bp(BP_ALPHA_C0, AC(5, 6, 3, 1) | 1u << 18 | 2u << 16 | 3u << 20);
    rect4(430, 140, 200, 100, 0, 0x102030FFu, 0xF0E0D080u, 0x80808000u, 0x00FF00FFu);
    /* Comparación: GR16 mayor y RGB8 igual */
    gx_bp(BP_COLOR_C0, CC(10, 14, 12, 2) | 3u << 16 | 1u << 20);
    gx_bp(BP_ALPHA_C0, AC(5, 6, 6, 1) | 3u << 16 | 3u << 20);
    rect4(10, 250, 300, 100, 0, 0x409000FFu, 0x40900000u, 0x3F90E0FFu, 0x41FF00FFu);
    gx_bp(BP_COLOR_C0, CC(10, 14, 12, 15) | 3u << 16 | 1u << 18 | 3u << 20);
    rect4(320, 250, 300, 100, 0, 0x409000FFu, 0x40900000u, 0x40E0E0FFu, 0x40FF00FFu);
    /* d de 11 bits: C1 = 600 con recorte da 255; resta en alfa con c que no
     * es 0 ni 255 (el redondeo de -temp >> 8) */
    gx_bp(BP_TEV_RA + 4, 600 | 0x0F0u << 12);
    gx_bp(BP_TEV_BG + 4, 0x7A0 | 0x258u << 12);         /* b -96, g 600 */
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 4));
    gx_bp(BP_ALPHA_C0, AC(7, 5, 5, 6) | 1u << 18);
    gx_bp(BP_ZCONTROL, PF_RGBA6);                       /* para que el alfa quede */
    rect4(10, 470, 300, 40, 0, 0x00000010u, 0x000000F0u, 0x00000077u, 0x000000C3u);
    gx_bp(BP_ZCONTROL, PF_RGB8);
    /* Las 32 constantes: color y alfa con el mismo KonstSel */
    gx_bp(BP_TEV_RA + 2, 0x011 | 0x0EEu << 12 | 1u << 23);     /* K1 */
    gx_bp(BP_TEV_BG + 2, 0x033 | 0x022u << 12 | 1u << 23);
    gx_bp(BP_TEV_RA + 4, 0x044 | 0x0DDu << 12 | 1u << 23);     /* K2 */
    gx_bp(BP_TEV_BG + 4, 0x066 | 0x055u << 12 | 1u << 23);
    gx_bp(BP_TEV_RA + 6, 0x077 | 0x0CCu << 12 | 1u << 23);     /* K3 */
    gx_bp(BP_TEV_BG + 6, 0x099 | 0x088u << 12 | 1u << 23);
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 14));
    gx_bp(BP_ALPHA_C0, AC(7, 7, 7, 6));
    for (uint32_t k = 0; k < 32; k++) {
        gx_bp(BP_KSEL, 1u << 2 | k << 4 | k << 9);
        rect(320 + 9 * (float)(k % 16), 470 + 20 * (float)(k / 16), 8, 18, 0, 0xFFFFFFFFu);
    }
    gx_bp(BP_KSEL, 1u << 2);
    /* Dos etapas, la segunda con el canal 1 y la tabla de swap 1 (b g r a) */
    gx_bp(BP_GENMODE, 2u << 4 | 1u << 10);
    gx_xf1(XF_NUMCHAN, 2);
    gx_bp(BP_KSEL + 2, 2u | 1u << 2);
    gx_bp(BP_KSEL + 3, 0u | 3u << 2);
    gx_bp(BP_TEVORDER, 0 | 1u << 19);
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 10));
    gx_bp(BP_ALPHA_C0, AC(7, 7, 7, 5));
    gx_bp(BP_COLOR_C0 + 2, CC(0, 10, 13, 15));
    gx_bp(BP_ALPHA_C0 + 2, AC(0, 5, 7, 7) | 1u << 0);
    gx_begin(GX_QUADS, 0, 4);
    gx_f32(10); gx_f32(360); gx_f32(0); gx_u32(0xFF0000FFu); gx_u32(0x00FF0080u); gx_f32(0); gx_f32(0);
    gx_f32(310); gx_f32(360); gx_f32(0); gx_u32(0x00FF00FFu); gx_u32(0x0000FFFFu); gx_f32(0); gx_f32(0);
    gx_f32(310); gx_f32(460); gx_f32(0); gx_u32(0x0000FFFFu); gx_u32(0xFF0000FFu); gx_f32(0); gx_f32(0);
    gx_f32(10); gx_f32(460); gx_f32(0); gx_u32(0xFFFFFFFFu); gx_u32(0x80808080u); gx_f32(0); gx_f32(0);
    snap(0, 130, 640, 398);
    gx_xf1(XF_NUMCHAN, 1);
    gx_bp(BP_KSEL, 1u << 2);
    pixel_setup();
    sync_gpu("11. formatos, prueba de alfa y TEV");
}

/* ---- 12. el CPU lee el EFB y la caja envolvente ---------------------------------------- */

static uint32_t peek32(uint32_t x, uint32_t y, uint32_t z)
{
    return *(volatile uint32_t *)(0xC8000000u | x << 2 | y << 12 | z << 22);
}

static void bbox_read(void)
{
    for (uint32_t k = 0; k < 4; k++)
        peeks += (uint32_t)PE_REG(0x10 + 2 * k) << (4 * k);
}

static void cpu_access(void)
{
    pixel_setup();
    pixel_state(PF_RGBA6, ZMODE(Z_ALWAYS, 1), B_COLOR | B_ALPHA);
    clear_efb(0x00000000u, 0);
    rect4(0, 0, 64, 32, 0x345678, 0xFF0000FFu, 0x00FF0080u, 0x0000FF40u, 0xFFFFFF00u);
    sync_gpu("12. dibujo para leer");
    for (uint32_t mode = 0; mode < 3; mode++) {
        PE_REG(0x08) = (uint16_t)mode;
        for (uint32_t k = 0; k < 8; k++)
            peeks += peek32(k * 9, k * 4, 0) * (k + 1) + peek32(k * 9, k * 4, 1);
    }
    PE_REG(0x08) = 0;
    /* 8, 16 y 64 bits; fuera del EFB; Z + color; y escribir no hace nada */
    peeks += *(volatile uint8_t *)(0xC8000000u | 5u << 2 | 3u << 12);
    peeks += *(volatile uint16_t *)(0xC8400000u | 5u << 2 | 3u << 12);
    union { double d; uint32_t u[2]; } wide;
    wide.d = *(volatile double *)(0xC8000000u | 6u << 2 | 3u << 12);    /* lfd: los 32 bits abajo */
    peeks += wide.u[0] * 3 + wide.u[1];
    peeks += peek32(700, 10, 0) + peek32(10, 600, 1) + *(volatile uint32_t *)(0xC8800000u | 4u << 2);
    *(volatile uint32_t *)(0xC8000000u | 1u << 2 | 1u << 12) = 0x12345678u;
    *(volatile uint32_t *)(0xC8400000u | 1u << 2 | 1u << 12) = 0x123456u;
    peeks += peek32(1, 1, 0) + peek32(1, 1, 1);
    pixel_state(PF_RGB8, ZMODE(Z_ALWAYS, 1), B_COLOR | B_ALPHA);
    sync_gpu("12. formato RGB8");
    peeks += peek32(3, 3, 0) + peek32(3, 3, 1);

    /* Caja envolvente: como el SDK, 1023 0 1023 0, dibujar y leer */
    gx_bp(BP_BBOX0, 1023 | 0u << 10);
    gx_bp(BP_BBOX1, 1023 | 0u << 10);
    rect(33, 17, 50, 21, 0, 0xFFFFFFFFu);
    rect(100.5f, 200.5f, 1, 1, 0, 0xFFFFFFFFu);
    sync_gpu("12. caja envolvente");
    bbox_read();
    /* Sin volver a escribirla: queda apagada y la lectura no cambia */
    rect(300, 300, 10, 10, 0, 0xFFFFFFFFu);
    sync_gpu("12. caja sin escribir");
    bbox_read();
    /* Escrita y leída sin dibujar: lo pendiente; después de leerla, lo
     * escrito ya no llega al renderizador */
    gx_bp(BP_BBOX0, 10 | 300u << 10);
    gx_bp(BP_BBOX1, 20 | 400u << 10);
    sync_gpu("12. caja escrita");
    bbox_read();
    rect(400, 100, 10, 10, 0, 0xFFFFFFFFu);
    gx_bp(BP_BBOX0, 500 | 501u << 10);
    rect(410, 110, 10, 10, 0, 0xFFFFFFFFu);
    sync_gpu("12. caja escrita y dibujada");
    bbox_read();
    snap(0, 0, 640, 528);
    sync_gpu("12. lecturas del EFB");
    OSReport("gx_raster: suma de las lecturas %08x\n", (unsigned)peeks);
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
    fill();
    scissors();
    depth();
    blending();
    formats_alpha_tev();
    cpu_access();
    OSReport("gx_raster: %u tokens; %s\n", (unsigned)token, fails ? "FALLO" : "OK");
    exit(fails ? 1 : 0);
}
