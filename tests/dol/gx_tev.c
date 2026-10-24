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
 * DOL de prueba "gx_tev" (Fase 2, etapa C5): el TEV con la mini-librería
 * propia (gxlib y gxvtx, sin libogc).
 *
 *   1-4. configuraciones al azar (semilla fija), 80 por parte, de 1 a 16
 *        etapas: registros, constantes, tablas de swap, entradas, sesgo,
 *        escala, resta, comparaciones, recorte y destino; la parte 2 suma
 *        texturas y coordenadas, la 3 etapas indirectas y la 4 la prueba de
 *        alfa, la Z temprana y tardía, la textura de Z y la niebla. La mitad
 *        de los casos agrega una etapa al final que pasa el alfa al color
 *        (el EFB es RGB8);
 *   5. indirectas con texturas armadas: los 4 formatos, las 3 matrices y las
 *      dinámicas S y T, envolturas, la suma con la etapa anterior, el alfa
 *      de bump y 4 etapas indirectas;
 *   6. textura de Z: U8, U16 y U24, sumada y reemplazando, con Z temprana y
 *      tardía y la textura con swap; el CPU lee la Z que quedó;
 *   7. niebla: los 8 tipos en ortográfica y en perspectiva, el denominador
 *      que pasa por 0, A = 0, el caso NaN y el ajuste por rango;
 *   8. prueba de alfa: las 8 × 8 comparaciones con las 4 lógicas;
 *   9. comparaciones del TEV en los 4 modos, en color y en alfa, y el
 *      recorte a -1024.
 *
 * Los valores que en Dolphin son un PanicAlert o un ASSERT no se usan: canal
 * de color 2-4, matriz indirecta 3 (y una matriz sin índice), envoltura
 * indirecta 7, formato de textura de Z 3, más de 2 canales o 4 etapas
 * indirectas, formatos o envolturas de textura inválidos, y una matriz
 * dinámica (S o T) en una etapa cuya coordenada no existe si su etapa
 * indirecta existe: eso último lo revisa el generador de shaders, que corre
 * aunque el video sea por software.
 *
 * Se compara contra el renderizador por software de Dolphin: cada triángulo y
 * el CRC del EFB en una copia sin destino después de cada caso
 * (tests/dol/dolphin/gx_tev.cop.txt).
 *
 * Solo para el DOL: no compila en la PC.
 */
#include <stddef.h>

#include "gxlib.h"
#include "rt.h"

#define FIFO_SIZE (64u * 1024u)
#define MAX_WAIT  2000000u
#define CASES     80u

static uint8_t fifo[FIFO_SIZE] __attribute__((aligned(32)));
static uint8_t pool[96u * 1024u] __attribute__((aligned(32)));
static uint32_t pool_used;

static uint32_t token;
static int fails;
static uint32_t peeks;                          /* suma de las lecturas de Z */

enum {
    BP_INDMTX = 0x06, BP_TEVIND = 0x10, BP_SCISSOR_TL = 0x20, BP_SCISSOR_BR = 0x21, BP_TEXSCALE = 0x25,
    BP_IREF = 0x27, BP_TEVORDER = 0x28, BP_ZMODE = 0x40, BP_BLEND = 0x41, BP_ZCONTROL = 0x43,
    BP_COPY_XY = 0x49, BP_COPY_WH = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D, BP_CLEAR_AR = 0x4F,
    BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51, BP_COPY = 0x52, BP_SCISSOR_OFFSET = 0x59, BP_TLUT_SRC = 0x64,
    BP_TLUT_DEST = 0x65, BP_COMBINER = 0xC0, BP_TEV_RA = 0xE0, BP_TEV_BG = 0xE1, BP_FOGRANGE = 0xE8,
    BP_FOG_A = 0xEE, BP_FOG_BMAG = 0xEF, BP_FOG_BSHIFT = 0xF0, BP_FOG_C = 0xF1, BP_FOG_COLOR = 0xF2,
    BP_ALPHATEST = 0xF3, BP_ZTEX1 = 0xF4, BP_ZTEX2 = 0xF5, BP_KSEL = 0xF6,
};
enum {
    TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6, TF_C4 = 8,
    TF_C8 = 9, TF_C14X2 = 10, TF_CMPR = 14,
};
enum { WRAP_CLAMP = 0, WRAP_REPEAT = 1, WRAP_MIRROR = 2 };
enum { Z_NEVER = 0, Z_LESS = 1, Z_EQUAL = 2, Z_LEQUAL = 3, Z_GREATER = 4, Z_NEQUAL = 5, Z_GEQUAL = 6,
       Z_ALWAYS = 7 };
#define ZMODE(func, update) (1u | (uint32_t)(func) << 1 | (uint32_t)(update) << 4)
#define B_COLOR_ALPHA (1u << 3 | 1u << 4)

#define MODE0(ws, wt, mag, mip, min, diag, bias)                                                   \
    ((uint32_t)(ws) | (uint32_t)(wt) << 2 | (uint32_t)(mag) << 4 | (uint32_t)(mip) << 5 |          \
     (uint32_t)(min) << 7 | (uint32_t)(diag) << 8 | ((uint32_t)(bias) & 0xFFu) << 9)
#define MODE1(minlod, maxlod) ((uint32_t)(minlod) | (uint32_t)(maxlod) << 8)
/* Combinadores: entradas a, b, c, d; recorte a 0-255, sin sesgo ni escala, a PREV */
#define CC(a, b, c, d) ((uint32_t)(a) << 12 | (uint32_t)(b) << 8 | (uint32_t)(c) << 4 | (uint32_t)(d) | 1u << 19)
#define AC(a, b, c, d) ((uint32_t)(a) << 13 | (uint32_t)(b) << 10 | (uint32_t)(c) << 7 | (uint32_t)(d) << 4 | 1u << 19)
/* Argumentos de color y de alfa que se usan en las partes dirigidas */
enum { CA_CPREV = 0, CA_TEXC = 8, CA_TEXA = 9, CA_RASC = 10, CA_RASA = 11, CA_HALF = 13, CA_ZERO = 15 };
enum { AA_APREV = 0, AA_TEXA = 4, AA_RASA = 5, AA_ZERO = 7 };
/* TevStageIndirect: etapa, formato, sesgo STU, alfa de bump, matriz, envolturas, suma */
#define IND(bt, fmt, bias, bs, mi, mid, sw, tw, add)                                               \
    ((uint32_t)(bt) | (uint32_t)(fmt) << 2 | (uint32_t)(bias) << 4 | (uint32_t)(bs) << 7 |         \
     (uint32_t)(mi) << 9 | (uint32_t)(mid) << 11 | (uint32_t)(sw) << 13 | (uint32_t)(tw) << 16 |     \
     (uint32_t)(add) << 20)
#define ALWAYS_PASS (7u << 16 | 7u << 19)

static const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
/* Ortográfica en píxeles exactos (como gx_raster): x, y del EFB y z de 24 bits */
static const float ortho_px[6] = { 1.0f / 512.0f, -1.0f, -1.0f / 512.0f, 1.0f, 1.0f / 16777216.0f, -1.0f };
static const float viewport_px[6] = { 512.0f, -512.0f, 16777216.0f, 854.0f, 854.0f, 16777216.0f };

/* ---- utilidades -------------------------------------------------------------------- */

/* Espera a la GPU; con what, además lo anota */
static void wait_gpu(const char *what)
{
    token++;
    gx_bp(BP_TOKEN, token);
    gx_flush();
    if (!gx_wait_idle(MAX_WAIT)) {
        OSReport("gx_tev: %s: la GPU no vacio el FIFO\n", what ? what : "caso");
        fails++;
        return;
    }
    uint32_t n = 0;
    while (PE_REG(PE_TOKEN) != (uint16_t)token && n < MAX_WAIT)
        n++;
    if (n == MAX_WAIT) {
        OSReport("gx_tev: %s: el token %u no llego\n", what ? what : "caso", (unsigned)token);
        fails++;
        return;
    }
    if (what)
        OSReport("gx_tev: %s\n", what);
}

static uint32_t seed = 0x6C8E9CF5u;

/* 24 bits al azar */
static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

static uint32_t rnd_n(uint32_t n)
{
    return rnd() % n;
}

static uint32_t rnd32(void)
{
    return rnd() << 8 ^ rnd();
}

static float rnd_f(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rnd() & 0xFFFFu) / 65536.0f;
}

static uint8_t *alloc(uint32_t n)
{
    uint8_t *p = pool + pool_used;
    pool_used += (n + 31u) & ~31u;
    if (pool_used > sizeof pool) {
        OSReport("gx_tev: sin lugar para las texturas\n");
        fails++;
        exit(1);
    }
    return p;
}

static uint8_t *random_bytes(uint32_t n)
{
    uint8_t *p = alloc(n);
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)rnd();
    dc_flush(p, n);
    return p;
}

static uint32_t block_w(uint32_t fmt)
{
    return (fmt == TF_I4 || fmt == TF_C4 || fmt == TF_CMPR || fmt == TF_I8 || fmt == TF_IA4 || fmt == TF_C8) ? 8 : 4;
}

static uint32_t block_h(uint32_t fmt)
{
    return (fmt == TF_I4 || fmt == TF_C4 || fmt == TF_CMPR) ? 8 : 4;
}

/* Bytes de una textura (con el relleno del bloque) */
static uint32_t tex_bytes(uint32_t fmt, uint32_t w, uint32_t h)
{
    const uint32_t bpb = fmt == TF_RGBA8 ? 64 : 32;
    return ((w + block_w(fmt) - 1) / block_w(fmt)) * ((h + block_h(fmt) - 1) / block_h(fmt)) * bpb;
}

/* Un texel de una textura armada a mano (I8 o IA8: bytes por texel) */
static void put_texel(uint8_t *t, uint32_t fmt, uint32_t w, uint32_t x, uint32_t y, uint32_t bpt, uint32_t v)
{
    const uint32_t bw = block_w(fmt), bh = block_h(fmt), nbx = (w + bw - 1) / bw;
    uint8_t *p = t + (((y / bh) * nbx + x / bw) * bw * bh + (y % bh) * bw + x % bw) * bpt;
    if (bpt == 2) {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else {
        p[0] = (uint8_t)v;
    }
}

/* Onda triangular de 0 a 255 con período p */
static uint32_t wave(uint32_t x, uint32_t p)
{
    const uint32_t k = (2 * 255 * (x % p)) / p;
    return k <= 255 ? k : 510 - k;
}

static void genmode(uint32_t texgens, uint32_t chans, uint32_t stages, uint32_t ind)
{
    gx_bp(BP_GENMODE, texgens | chans << 4 | (stages - 1) << 10 | ind << 16);
    gx_xf1(XF_NUMTEXGEN, texgens);
    gx_xf1(XF_NUMCHAN, chans);
}

/* La unidad de textura map: modos, tamaño, formato, datos en la RAM y paleta */
static void tex_unit(uint32_t map, uint32_t mode0, uint32_t mode1, uint32_t w, uint32_t h, uint32_t fmt,
                     const void *data, uint32_t tlutfmt)
{
    const uint32_t r = 0x80u | (map & 3u) | (map & 4u) << 3;
    gx_bp(r + 0x00, mode0);
    gx_bp(r + 0x04, mode1);
    gx_bp(r + 0x08, (w - 1) | (h - 1) << 10 | fmt << 20);
    gx_bp(r + 0x0C, 0);                         /* desde la RAM */
    gx_bp(r + 0x14, gx_phys(data) >> 5);
    gx_bp(r + 0x18, 512 | tlutfmt << 10);       /* la paleta de setup */
}

/* La matriz de la coordenada i (en la memoria de XF, índice 30 + 3i) */
static void tex_matrix(uint32_t i, const float m[8])
{
    gx_xf_floats((30 + 3 * i) * 4, 8, m);
}

static void su_size(uint32_t coord, uint32_t w, uint32_t h)
{
    gx_bp(BP_SU_SSIZE + 2 * coord, w - 1);
    gx_bp(BP_SU_TSIZE + 2 * coord, h - 1);
}

/* Registro i del TEV (konst = 0) o constante i (konst = 1), con 11 bits con signo */
static void tev_reg(uint32_t i, uint32_t konst, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    gx_bp(BP_TEV_RA + 2 * i, (r & 0x7FFu) | (a & 0x7FFu) << 12 | konst << 23);
    gx_bp(BP_TEV_BG + 2 * i, (b & 0x7FFu) | (g & 0x7FFu) << 12 | konst << 23);
}

/* Un float de los registros de la niebla: signo, exponente y 11 bits de mantisa */
static uint32_t fog_float(float f)
{
    const uint32_t v = f32_bits(f);
    return (v >> 31) << 19 | ((v >> 23) & 0xFFu) << 11 | ((v >> 12) & 0x7FFu);
}

/* Niebla: tipo, proyección (1 = ortográfica), A, B (magnitud y corrimiento), C y color */
static void fog(uint32_t fsel, uint32_t ortho, uint32_t a, uint32_t bmag, uint32_t bshift, uint32_t c,
                uint32_t color)
{
    gx_bp(BP_FOG_A, a);
    gx_bp(BP_FOG_BMAG, bmag);
    gx_bp(BP_FOG_BSHIFT, bshift);
    gx_bp(BP_FOG_C, c | ortho << 20 | fsel << 21);
    gx_bp(BP_FOG_COLOR, color);
}

static void v(float x, float y, float z, uint32_t c0, uint32_t c1, float s, float t)
{
    gx_f32(x);
    gx_f32(y);
    gx_f32(z);
    gx_u32(c0);
    gx_u32(c1);
    gx_f32(s);
    gx_f32(t);
}

/* Un rectángulo de dos triángulos con z de izquierda a derecha y la
 * coordenada 0 de (0, 0) a (su, tu) */
static void quad(float x, float y, float w, float h, float z0, float z1, uint32_t c, float su, float tu)
{
    gx_begin(GX_QUADS, 0, 4);
    v(x, y, z0, c, c, 0, 0);
    v(x + w, y, z1, c, c, su, 0);
    v(x + w, y + h, z1, c, c, su, tu);
    v(x, y + h, z0, c, c, 0, tu);
}

static void copy_efb(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t trigger)
{
    gx_bp(BP_COPY_XY, x | y << 10);
    gx_bp(BP_COPY_WH, (w - 1) | (h - 1) << 10);
    gx_bp(BP_COPY_DST, 0);
    gx_bp(BP_COPY_STRIDE, 0);
    gx_bp(BP_COPY, trigger);
}

/* Copia sin destino: Dolphin y el runner anotan el CRC del rectángulo */
static void snap(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    copy_efb(x, y, w, h, 0);
    wait_gpu(NULL);
}

static void clear_efb(uint32_t rgba, uint32_t z)
{
    gx_bp(BP_ZMODE, ZMODE(Z_ALWAYS, 1));
    gx_bp(BP_CLEAR_AR, (rgba & 0xFFu) << 8 | rgba >> 24);
    gx_bp(BP_CLEAR_GB, (rgba >> 8) & 0xFFFFu);
    gx_bp(BP_CLEAR_Z, z);
    copy_efb(0, 0, 640, 528, 1u << 11);
    gx_bp(BP_ZMODE, 0);
}

static uint32_t peek_z(uint32_t x, uint32_t y)
{
    return *(volatile uint32_t *)(0xC8000000u | x << 2 | y << 12 | 1u << 22);
}

/* ---- 0. estado inicial -------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    uint32_t fmt, w, h;
} texture_t;

#define NTEX 16u
static texture_t texs[NTEX];

/* El estado de las partes dirigidas: una etapa que pasa el color del
 * rasterizador, sin indirectas, sin niebla ni textura de Z, swap normal y
 * las coordenadas sin transformar */
static void base_state(uint32_t texgens, uint32_t stages, uint32_t ind)
{
    static const float tm[8] = { 1, 0, 0, 0, 0, 1, 0, 0 };
    genmode(texgens, 1, stages, ind);
    for (uint32_t i = 0; i < 8; i++) {
        tex_matrix(i, tm);
        gx_bp(BP_KSEL + i, (i & 1u) ? (2u | 3u << 2) : (0u | 1u << 2));
        gx_bp(BP_TEVORDER + i, 0);
    }
    for (uint32_t s = 0; s < 16; s++) {
        gx_bp(BP_TEVIND + s, 0);
        gx_bp(BP_COMBINER + 2 * s, CC(CA_ZERO, CA_ZERO, CA_ZERO, CA_RASC));
        gx_bp(BP_COMBINER + 2 * s + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_RASA));
    }
    for (uint32_t i = 0; i < 4; i++) {
        tev_reg(i, 0, 0, 0, 0, 0);
        tev_reg(i, 1, 0, 0, 0, 0);
    }
    for (uint32_t i = 0; i < 9; i++)
        gx_bp(BP_INDMTX + i, 0);
    gx_bp(BP_TEXSCALE, 0);
    gx_bp(BP_TEXSCALE + 1, 0);
    gx_bp(BP_IREF, 0);
    gx_bp(BP_ZTEX1, 0);
    gx_bp(BP_ZTEX2, 0);
    fog(0, 0, 0, 0, 0, 0, 0);
    for (uint32_t i = 0; i < 6; i++)
        gx_bp(BP_FOGRANGE + i, 0);
    gx_bp(BP_ALPHATEST, ALWAYS_PASS);
    gx_bp(BP_ZCONTROL, 0);                      /* RGB8, Z tardía */
    gx_bp(BP_ZMODE, 0);
    gx_bp(BP_BLEND, B_COLOR_ALPHA);
}

static void setup(void)
{
    gx_init_fifo(fifo, FIFO_SIZE, FIFO_SIZE - 16384, FIFO_SIZE / 2, CP_CR_READ | CP_CR_LINK);
    gx_xf1(XF_DUALTEX, 0);
    gx_projection(ortho_px, 1);
    gx_xf_floats(XF_VIEWPORT, 6, viewport_px);
    gx_pos_matrix(0, ident);
    /* Las 8 coordenadas salen de la 0 del vértice, cada una con su matriz */
    gx_matindex(0 | 30u << 6 | 33u << 12 | 36u << 18 | 39u << 24, 42u | 45u << 6 | 48u << 12 | 51u << 18);
    gx_xf1(XF_COLOR0, CH_MATSRC_VTX);
    gx_xf1(XF_COLOR0 + 1, CH_MATSRC_VTX);
    gx_xf1(XF_ALPHA0, CH_MATSRC_VTX);
    gx_xf1(XF_ALPHA0 + 1, CH_MATSRC_VTX);
    for (uint32_t i = 0; i < 8; i++)
        gx_xf1(XF_TEXMTXINFO + i, TG_ROW(5));
    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_8888) | VAT_COL1(COL_8888) | VAT_TEX0(1, FMT_F32, 0), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT) | VCD_COL1(VTX_DIRECT), VCD_TEX(0, VTX_DIRECT), 0);

    gx_bp(BP_SCISSOR_TL, 342 | 342u << 12);     /* tijera en todo el EFB */
    gx_bp(BP_SCISSOR_BR, (342 + 527) | (342u + 639) << 12);
    gx_bp(BP_SCISSOR_OFFSET, 171 | 171u << 10);

    /* Una paleta de 16384 entradas al azar, a la TMEM desde la línea 512 */
    const uint8_t *tlut = random_bytes(32768);
    gx_bp(BP_TLUT_SRC, gx_phys(tlut) >> 5);
    gx_bp(BP_TLUT_DEST, 512 | 1024u << 10);

    /* Texturas al azar en los 11 formatos */
    static const uint32_t fmts[11] = { TF_I4, TF_I8, TF_IA4, TF_IA8, TF_RGB565, TF_RGB5A3, TF_RGBA8, TF_C4,
                                       TF_C8, TF_C14X2, TF_CMPR };
    for (uint32_t i = 0; i < NTEX; i++) {
        texs[i].fmt = fmts[i % 11];
        texs[i].w = 2 + rnd_n(31);
        texs[i].h = 2 + rnd_n(31);
        texs[i].data = random_bytes(tex_bytes(texs[i].fmt, texs[i].w, texs[i].h));
    }
    base_state(0, 1, 0);
    wait_gpu("estado inicial");
}

/* ---- 1-4. al azar ------------------------------------------------------------------- */

enum { F_TEX = 1, F_IND = 2, F_PIX = 4 };

/* -1024 a 1023, casi siempre 0 a 255 */
static uint32_t rnd_s11(void)
{
    return (rnd() & 3u) ? rnd_n(256) : rnd() & 0x7FFu;
}

/* Un combinador de color o de alfa: entradas (y tablas de swap) al azar;
 * d en cero, suma, sin sesgo, sin escala y con recorte más seguido, para
 * que no sature */
static uint32_t rnd_comb(int alpha)
{
    uint32_t in = rnd() & 0xFFFFu;
    if (rnd_n(4))
        in |= alpha ? 7u << 4 : 15u;
    const uint32_t bias = rnd_n(4) ? 0 : rnd_n(4), scale = rnd_n(4) ? 0 : rnd_n(4);
    return in | bias << 16 | (uint32_t)(rnd_n(8) == 0) << 18 | (uint32_t)(rnd_n(4) != 0) << 19 |
           scale << 20 | rnd_n(4) << 22;
}

/* La etapa indirecta de una etapa con la coordenada tc */
static uint32_t rnd_tevind(uint32_t tc, uint32_t ntg, uint32_t nind)
{
    const uint32_t bt = rnd_n(4), mi = rnd_n(4);
    uint32_t mid = mi ? rnd_n(3) : 0;
    if (tc >= ntg && bt < nind)
        mid = 0;
    return IND(bt, rnd_n(4), rnd_n(8), rnd_n(4), mi, mid, rnd_n(7), rnd_n(7), rnd_n(2));
}

static void random_textures(void)
{
    for (uint32_t map = 0; map < 8; map++) {
        const texture_t *t = &texs[rnd_n(NTEX)];
        const uint32_t minlod = rnd_n(16);
        tex_unit(map, MODE0(rnd_n(3), rnd_n(3), rnd_n(2), 0, rnd_n(2), rnd_n(2), rnd()),
                 MODE1(minlod, minlod + rnd_n(256 - minlod)), t->w, t->h, t->fmt, t->data, rnd_n(3));
    }
    for (uint32_t i = 0; i < 8; i++)
        su_size(i, 1 + rnd_n(64), 1 + rnd_n(64));
}

static void random_fog(void)
{
    const uint32_t ortho = rnd_n(2);
    const float a = ortho ? rnd_f(0.25f, 4.0f) : rnd_f(0.01f, 0.5f);
    fog(rnd_n(8), ortho, fog_float(a), rnd(), rnd_n(12), fog_float(rnd_f(-1.0f, 1.0f)), rnd());
    gx_bp(BP_FOGRANGE, rnd_n(2) ? (342 + rnd_n(640)) | 1u << 10 : 0);
    for (uint32_t i = 0; i < 5; i++)
        gx_bp(BP_FOGRANGE + 1 + i, rnd());
}

static void random_case(uint32_t feat, uint32_t cell)
{
    static const uint32_t chans[7] = { 0, 0, 1, 1, 5, 6, 7 };
    const float x = (float)(32 * (cell % 20)), y = (float)(32 * (cell / 20));
    uint32_t nst = 1 + rnd_n(16);
    const uint32_t nch = rnd_n(3);
    const uint32_t ntg = (feat & F_TEX) ? rnd_n(9) : 0;
    const uint32_t nind = (feat & F_IND) ? rnd_n(5) : 0;

    for (uint32_t i = 0; i < 4; i++) {
        tev_reg(i, 1, rnd_s11(), rnd_s11(), rnd_s11(), rnd_s11());
        tev_reg(i, 0, rnd_s11(), rnd_s11(), rnd_s11(), rnd_s11());
    }
    for (uint32_t i = 0; i < 8; i++)
        gx_bp(BP_KSEL + i, rnd());

    uint32_t order[8] = { 0 }, ac = 0;
    for (uint32_t s = 0; s < nst; s++) {
        const uint32_t tc = rnd_n(8);
        const uint32_t o = rnd_n(8) | tc << 3 | (uint32_t)((feat & F_TEX) && rnd_n(4)) << 6 | chans[rnd_n(7)] << 7;
        order[s / 2] |= o << (12 * (s & 1u));
        ac = rnd_comb(1);
        gx_bp(BP_COMBINER + 2 * s, rnd_comb(0));
        gx_bp(BP_COMBINER + 2 * s + 1, ac);
        gx_bp(BP_TEVIND + s, (feat & F_IND) ? rnd_tevind(tc, ntg, nind) : 0);
    }
    /* La mitad de las veces, una etapa más con el alfa como color */
    if (nst < 16 && rnd_n(2)) {
        const uint32_t adest = (ac >> 22) & 3u;
        order[nst / 2] |= 7u << (7 + 12 * (nst & 1u));
        gx_bp(BP_COMBINER + 2 * nst, CC(CA_ZERO, CA_ZERO, CA_ZERO, 2 * adest + 1));
        gx_bp(BP_COMBINER + 2 * nst + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, adest));
        gx_bp(BP_TEVIND + nst, 0);
        nst++;
    }
    for (uint32_t i = 0; i < 8; i++)
        gx_bp(BP_TEVORDER + i, order[i]);

    if (feat & F_TEX) {
        random_textures();
        for (uint32_t i = 0; i < ntg; i++) {
            const float m[8] = { rnd_f(-2, 2), rnd_f(-2, 2), rnd_f(-2, 2), 0,
                                 rnd_f(-2, 2), rnd_f(-2, 2), rnd_f(-2, 2), 0 };
            tex_matrix(i, m);
        }
    }
    if (feat & F_IND) {
        for (uint32_t i = 0; i < 9; i++)
            gx_bp(BP_INDMTX + i, rnd());
        gx_bp(BP_IREF, rnd());
        gx_bp(BP_TEXSCALE, rnd() & 0xFFFFu);
        gx_bp(BP_TEXSCALE + 1, rnd() & 0xFFFFu);
    }
    if (feat & F_PIX) {
        /* La prueba de alfa y la Z al azar una de cada 3 y 2 veces: si no,
         * la mitad de los casos no dibuja nada */
        gx_bp(BP_ALPHATEST, rnd_n(3) ? ALWAYS_PASS : rnd());
        gx_bp(BP_ZTEX1, rnd());
        gx_bp(BP_ZTEX2, rnd_n(3) | rnd_n(3) << 2);
        gx_bp(BP_ZMODE, rnd_n(2) ? rnd() & 0x1Fu : 0);
        gx_bp(BP_ZCONTROL, rnd_n(2) << 6);
        if (rnd_n(2))
            random_fog();
        else
            gx_bp(BP_FOG_C, 0);
    }
    genmode(ntg, nch, nst, nind);

    const float z = (feat & F_PIX) ? -1.0f : 0x400000;
    gx_begin(GX_TRIANGLES, 0, 3);
    v(x + (float)rnd_n(8), y + (float)rnd_n(8), z < 0 ? (float)rnd() : z, rnd32(), rnd32(), rnd_f(-2, 3),
      rnd_f(-2, 3));
    v(x + 31 - (float)rnd_n(8), y + (float)rnd_n(12), z < 0 ? (float)rnd() : z, rnd32(), rnd32(),
      rnd_f(-2, 3), rnd_f(-2, 3));
    v(x + (float)rnd_n(12), y + 31 - (float)rnd_n(8), z < 0 ? (float)rnd() : z, rnd32(), rnd32(),
      rnd_f(-2, 3), rnd_f(-2, 3));
    snap((uint32_t)x, (uint32_t)y, 32, 32);
}

static void random_part(uint32_t feat, const char *what)
{
    clear_efb(0x182838FFu, 0x800000u);
    for (uint32_t c = 0; c < CASES; c++)
        random_case(feat, c);
    wait_gpu(what);
}

/* ---- 5. indirectas dirigidas ------------------------------------------------------- */

static uint8_t *board, *bump;
static const uint8_t *zmap;

/* Matriz indirecta i (0-2): elementos S0.10 y la escala con su +17 */
static void ind_matrix(uint32_t i, int32_t ma, int32_t mb, int32_t mc, int32_t md, int32_t me, int32_t mf,
                       uint32_t scale)
{
    gx_bp(BP_INDMTX + 3 * i, ((uint32_t)ma & 0x7FFu) | ((uint32_t)mb & 0x7FFu) << 11 | (scale & 3u) << 22);
    gx_bp(BP_INDMTX + 3 * i + 1, ((uint32_t)mc & 0x7FFu) | ((uint32_t)md & 0x7FFu) << 11 | (scale >> 2 & 3u) << 22);
    gx_bp(BP_INDMTX + 3 * i + 2, ((uint32_t)me & 0x7FFu) | ((uint32_t)mf & 0x7FFu) << 11 | (scale >> 4 & 1u) << 22);
}

/* Etapa s: la textura 0 con la coordenada 0, mezclada con PREV a la mitad */
static void stage_tex(uint32_t s, uint32_t first)
{
    gx_bp(BP_COMBINER + 2 * s, first ? CC(CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXC) : CC(CA_CPREV, CA_TEXC, CA_HALF, CA_ZERO));
    gx_bp(BP_COMBINER + 2 * s + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_TEXA));
}

static void ind_quad(uint32_t k)
{
    const uint32_t x = 4 + 68 * (k % 9), y = 4 + 68 * (k / 9);
    quad((float)x, (float)y, 64, 64, 0x400000, 0x400000, 0xC08040FFu, 2, 2);
    snap(x, y, 64, 64);
}

static void indirect_part(void)
{
    base_state(2, 1, 1);
    clear_efb(0x000000FFu, 0xFFFFFFu);
    /* Tablero I8 de 32 × 32 en la unidad 0 (repetido, bilineal) y el mapa de
     * bump IA8 de 16 × 16 en la 1: A es la onda en x, I en y */
    tex_unit(0, MODE0(WRAP_REPEAT, WRAP_REPEAT, 1, 0, 1, 0, 0), 0, 32, 32, TF_I8, board, 0);
    tex_unit(1, MODE0(WRAP_REPEAT, WRAP_REPEAT, 0, 0, 0, 0, 0), 0, 16, 16, TF_IA8, bump, 0);
    su_size(0, 32, 32);
    su_size(1, 16, 16);
    static const float half[8] = { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0 };
    tex_matrix(1, half);                        /* el bump, una vez por rectángulo */
    gx_bp(BP_TEVORDER, 0 | 0u << 3 | 1u << 6 | 7u << 7);
    stage_tex(0, 1);
    gx_bp(BP_IREF, 1 | 1u << 3);                /* indirecta 0: unidad 1, coordenada 1 */
    ind_matrix(0, 64, 0, 0, 64, 0, 0, 17);
    ind_matrix(1, 0, 48, -48, 0, 32, 32, 18);
    ind_matrix(2, 100, -30, 20, 90, -60, 10, 14);

    uint32_t k = 0;
    /* Los 4 formatos con sesgo, con las 3 matrices */
    for (uint32_t fmt = 0; fmt < 4; fmt++)
        for (uint32_t mi = 1; mi < 4; mi++, k++) {
            gx_bp(BP_TEVIND, IND(0, fmt, 7, 0, mi, 0, 0, 0, 0));
            ind_quad(k);
        }
    /* Sin sesgo en T, con U */
    gx_bp(BP_TEVIND, IND(0, 0, 5, 0, 1, 0, 0, 0, 0));
    ind_quad(k++);
    /* Las dinámicas S y T (s × índice / 256, corrido 2) */
    ind_matrix(0, 64, 0, 0, 64, 0, 0, 15);
    for (uint32_t mid = 1; mid < 3; mid++, k++) {
        gx_bp(BP_TEVIND, IND(0, 0, 7, 0, 1, mid, 0, 0, 0));
        ind_quad(k);
    }
    ind_matrix(0, 64, 0, 0, 64, 0, 0, 17);
    /* Envolturas de la coordenada, sin matriz */
    for (uint32_t w = 1; w < 7; w++, k++) {
        gx_bp(BP_TEVIND, IND(0, 0, 0, 0, 0, 0, w, 7 - w, 0));
        ind_quad(k);
    }
    /* Escala extrema: corre 14 a la izquierda (vuelta de 32 bits) y 17 a la derecha */
    ind_matrix(2, -1024, 1023, 1023, -1024, 7, -7, 31);
    gx_bp(BP_TEVIND, IND(0, 0, 7, 0, 3, 0, 0, 0, 0));
    ind_quad(k++);
    ind_matrix(2, -1024, 1023, 1023, -1024, 7, -7, 0);
    ind_quad(k++);
    ind_matrix(2, 100, -30, 20, 90, -60, 10, 14);

    /* Suma con la etapa anterior: la 0 desplaza, la 1 repite (ITW_0 y suma),
     * la 2 suma su coordenada otra vez */
    genmode(2, 1, 3, 1);
    gx_bp(BP_TEVORDER, (0 | 1u << 6 | 7u << 7) | (0 | 1u << 6 | 7u << 7) << 12);
    gx_bp(BP_TEVORDER + 1, 0 | 1u << 6 | 7u << 7);
    stage_tex(1, 0);
    stage_tex(2, 0);
    gx_bp(BP_TEVIND, IND(0, 0, 7, 0, 1, 0, 0, 0, 0));
    gx_bp(BP_TEVIND + 1, IND(0, 0, 0, 0, 0, 0, 6, 6, 1));
    gx_bp(BP_TEVIND + 2, IND(0, 1, 7, 0, 2, 0, 0, 0, 1));
    ind_quad(k++);
    gx_bp(BP_TEVIND + 2, IND(0, 1, 7, 0, 2, 0, 4, 3, 1));
    ind_quad(k++);
    for (uint32_t s = 0; s < 3; s++)
        gx_bp(BP_TEVIND + s, 0);

    /* Alfa de bump: S, T y U con los 4 formatos, al color por los canales 5 y 6 */
    genmode(2, 1, 1, 1);
    gx_bp(BP_COMBINER, CC(CA_ZERO, CA_ZERO, CA_ZERO, CA_RASC));
    gx_bp(BP_COMBINER + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_RASA));
    for (uint32_t bs = 1; bs < 4; bs++)
        for (uint32_t chan = 5; chan < 7; chan++, k++) {
            gx_bp(BP_TEVORDER, 0 | chan << 7);
            gx_bp(BP_TEVIND, IND(0, (bs + chan) & 3u, 0, bs, 0, 0, 0, 0, 0));
            ind_quad(k);
        }

    /* 4 etapas indirectas en 4 etapas del TEV, con escalas de coordenada y
     * una coordenada que no existe (vale la 0) */
    genmode(2, 1, 4, 4);
    gx_bp(BP_IREF, (1 | 1u << 3) | (1 | 0u << 3) << 6 | (0 | 1u << 3) << 12 | (1 | 5u << 3) << 18);
    gx_bp(BP_TEXSCALE, 1 | 2u << 4 | 0u << 8 | 3u << 12);
    gx_bp(BP_TEXSCALE + 1, 2 | 1u << 4 | 4u << 8 | 0u << 12);
    for (uint32_t s = 0; s < 4; s++) {
        stage_tex(s, s == 0);
        gx_bp(BP_TEVIND + s, IND(s, s, 7, s, 1 + s % 3, 0, 0, 0, 0));
    }
    gx_bp(BP_TEVORDER, (0 | 1u << 6 | 7u << 7) | (0 | 1u << 3 | 1u << 6 | 7u << 7) << 12);
    gx_bp(BP_TEVORDER + 1, (0 | 1u << 6 | 5u << 7) | (0 | 1u << 6 | 6u << 7) << 12);
    ind_quad(k++);
    wait_gpu("5. indirectas");
}

/* ---- 6. textura de Z ---------------------------------------------------------------- */

static void ztex_part(void)
{
    base_state(1, 1, 0);
    clear_efb(0x000000FFu, 0xFFFFFFu);
    tex_unit(0, MODE0(WRAP_REPEAT, WRAP_REPEAT, 0, 0, 0, 0, 0), 0, 16, 16, TF_RGB5A3, zmap, 0);
    su_size(0, 16, 16);
    gx_bp(BP_TEVORDER, 0 | 1u << 6 | 0u << 7);
    gx_bp(BP_COMBINER, CC(CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXC));
    /* La textura con la tabla de swap 1 (g b a r): la textura de Z usa el
     * texel sin swap */
    gx_bp(BP_KSEL + 2, 1 | 2u << 2);
    gx_bp(BP_KSEL + 3, 3 | 0u << 2);
    gx_bp(BP_COMBINER + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_TEXA) | 1u << 2);

    uint32_t k = 0;
    for (uint32_t type = 0; type < 3; type++)
        for (uint32_t op = 1; op < 3; op++)
            for (uint32_t early = 0; early < 2; early++, k++) {
                const uint32_t x = 4 + 52 * (k % 12), y = 4 + 52 * (k / 12) * 2;
                gx_bp(BP_ZTEX1, rnd() & 0x3FFFFFu);
                gx_bp(BP_ZTEX2, type | op << 2);
                gx_bp(BP_ZCONTROL, early << 6);
                gx_bp(BP_ZMODE, ZMODE(Z_ALWAYS, 1));
                quad((float)x, (float)y, 48, 48, 0x100000, 0xF00000, 0xFFFFFFFFu, 2, 2);
                snap(x, y, 48, 48);
                for (uint32_t j = 0; j < 3; j++)
                    for (uint32_t i = 0; i < 3; i++)
                        peeks += peek_z(x + 4 + 20 * i, y + 4 + 20 * j) * (1 + i + 3 * j + 9 * k);
                /* Lo que pasa contra esa Z: un plano en 0x800000 abajo */
                gx_bp(BP_ZTEX2, 0);
                gx_bp(BP_ZCONTROL, 0);
                gx_bp(BP_ZMODE, ZMODE(Z_LESS, 0));
                gx_bp(BP_TEVORDER, 0 | 0u << 7);
                gx_bp(BP_COMBINER, CC(CA_ZERO, CA_ZERO, CA_ZERO, CA_RASC));
                quad((float)x, (float)y, 48, 48, 0x800000, 0x800000, 0x40C0FFFFu, 1, 1);
                gx_bp(BP_TEVORDER, 0 | 1u << 6 | 0u << 7);
                gx_bp(BP_COMBINER, CC(CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXC));
                snap(x, y, 48, 48);
            }
    /* Apagada con el tipo 3: no se mira */
    gx_bp(BP_ZTEX2, 3);
    gx_bp(BP_ZMODE, ZMODE(Z_ALWAYS, 1));
    quad(4, 400, 48, 48, 0x100000, 0xF00000, 0xFFFFFFFFu, 2, 2);
    wait_gpu(NULL);
    peeks += peek_z(30, 420);
    gx_bp(BP_ZTEX2, 0);
    gx_bp(BP_ZMODE, 0);
    snap(0, 0, 640, 456);
    wait_gpu("6. textura de Z");
}

/* ---- 7. niebla ---------------------------------------------------------------------- */

static uint32_t fog_row;

/* Una fila con z de 0 a 0xFFFFFF, o fija */
static void fog_quad_z(float z0, float z1)
{
    const uint32_t y = 2 + 12 * fog_row++;
    quad(0, (float)y, 640, 10, z0, z1, 0x808080FFu, 1, 1);
    snap(0, y, 640, 10);
}

static void fog_quad(void)
{
    fog_quad_z(0, 16777215.0f);
}

static void fog_part(void)
{
    base_state(0, 1, 0);
    clear_efb(0x000000FFu, 0xFFFFFFu);
    fog_row = 0;
    const uint32_t one = fog_float(1.0f), zero = fog_float(0.0f);
    /* Los 8 tipos: ortográfica con A = 1, C = 0 y perspectiva de 1 a 16 */
    for (uint32_t fsel = 0; fsel < 8; fsel++) {
        fog(fsel, 1, one, 0, 0, zero, rnd());
        fog_quad();
    }
    for (uint32_t fsel = 0; fsel < 8; fsel++) {
        fog(fsel, 0, fog_float(0.0625f), 0x900000, 1, fog_float(0.1f), rnd());
        fog_quad();
    }
    /* El denominador pasa por 0 (y por negativo), con un corrimiento grande y A = 0 */
    fog(2, 0, fog_float(0.03f), 0x400000, 1, zero, rnd());
    fog_quad();
    fog(4, 0, fog_float(0.03f), 0x400000, 1, zero, rnd());
    fog_quad();
    fog(5, 0, fog_float(1.0f / 2097152.0f), 0x000020, 20, fog_float(-0.25f), rnd());
    fog_quad();
    fog(4, 0, zero, 0x400000, 1, fog_float(-0.5f), rnd());
    fog_quad();
    /* El denominador justo en 0: A / 0 es infinito, y 0 / 0 es NaN */
    fog(4, 0, fog_float(0.03f), 0x400000, 1, zero, rnd());
    fog_quad_z(0x800000, 0x800000);
    fog(2, 0, zero, 0x400000, 1, zero, rnd());
    fog_quad_z(0x800000, 0x800000);
    /* El caso NaN: A y C con exponente 255, con los 4 signos */
    for (uint32_t s = 0; s < 4; s++) {
        const uint32_t a = 0xFFu << 11 | (s & 1u) << 19 | 0x123u, c = 0xFFu << 11 | (s >> 1) << 19;
        fog(2, 1, a, 0, 0, c, rnd());
        fog_quad();
        fog(6, 0, a, 0x900000, 1, c | 0x400u, rnd());
        fog_quad();
    }
    /* Ajuste por rango: centro en el medio, a la izquierda y a la derecha;
     * el K[5] que no existe es el registro de A */
    static const uint32_t ranges[4][3] = { { 2, 1, 320 }, { 4, 0, 320 }, { 7, 1, 100 }, { 5, 0, 600 } };
    for (uint32_t r = 0; r < 4; r++) {
        for (uint32_t i = 0; i < 5; i++)
            gx_bp(BP_FOGRANGE + 1 + i, r < 2 ? (0x100u + 0x80u * i) << 12 | (0x140u + 0x80u * i) : rnd());
        gx_bp(BP_FOGRANGE, (342 + ranges[r][2]) | 1u << 10);
        if (ranges[r][1])
            fog(ranges[r][0], 1, one, 0, 0, fog_float(0.2f), rnd());
        else
            fog(ranges[r][0], 0, fog_float(0.0625f), 0x900000, 1, fog_float(0.1f), rnd());
        fog_quad();
    }
    gx_bp(BP_FOGRANGE, 0);
    gx_bp(BP_FOG_C, 0);
    wait_gpu("7. niebla");
}

/* ---- 8. prueba de alfa -------------------------------------------------------------- */

static void alpha_part(void)
{
    base_state(0, 1, 0);
    clear_efb(0x000000FFu, 0xFFFFFFu);
    for (uint32_t logic = 0; logic < 4; logic++)
        for (uint32_t c0 = 0; c0 < 8; c0++) {
            const uint32_t row = logic * 4 + c0 / 2;
            for (uint32_t c1 = 0; c1 < 8; c1++) {
                const float x = (float)(40 * ((c0 & 1u) * 8 + c1)), y = (float)(33 * row);
                gx_bp(BP_ALPHATEST, rnd_n(256) | rnd_n(256) << 8 | c0 << 16 | c1 << 19 | logic << 22);
                const uint32_t c = rnd32() & 0xFFFFFF00u;
                gx_begin(GX_TRIANGLES, 0, 3);
                v(x, y, 0, c, c, 0, 0);
                v(x + 38, y, 0, c | 0xFFu, c, 0, 0);
                v(x, y + 31, 0, c | 0x40u, c, 0, 0);
            }
            if (c0 & 1u)
                snap(0, 33 * row, 640, 33);
        }
    gx_bp(BP_ALPHATEST, ALWAYS_PASS);
    wait_gpu("8. prueba de alfa");
}

/* ---- 9. comparaciones y recorte --------------------------------------------------- */

static void quad4(float x, float y, float w, float h, uint32_t tl, uint32_t tr, uint32_t br, uint32_t bl)
{
    gx_begin(GX_QUADS, 0, 4);
    v(x, y, 0x400000, tl, tl, 0, 0);
    v(x + w, y, 0x400000, tr, tr, 0, 0);
    v(x + w, y + h, 0x400000, br, br, 0, 0);
    v(x, y + h, 0x400000, bl, bl, 0, 0);
}

static void compare_part(void)
{
    base_state(0, 2, 0);
    clear_efb(0x000000FFu, 0xFFFFFFu);
    /* Etapa 0: ras contra K0 = 128 en los 4 modos, mayor e igual, en color
     * (suma 255) y en alfa (suma A1 = 255); los modos de alfa R8, GR16 y
     * BGR24 comparan las entradas de color. Etapa 1: 3/4 del color y 1/4 del
     * alfa, para ver los dos */
    tev_reg(0, 1, 128, 128, 128, 128);
    tev_reg(2, 0, 0, 0, 0, 255);
    gx_bp(BP_KSEL, 1u << 2 | 12u << 4 | 28u << 9 | 6u << 14);
    gx_bp(BP_COMBINER + 2, CC(CA_CPREV, 1, 14, CA_ZERO));
    gx_bp(BP_COMBINER + 3, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_APREV));
    gx_bp(BP_TEVORDER, 0 | 0u << 7 | (7u << 7) << 12);
    for (uint32_t mode = 0; mode < 4; mode++)
        for (uint32_t eq = 0; eq < 2; eq++) {
            const uint32_t x = 4 + 68 * (mode * 2 + eq);
            gx_bp(BP_COMBINER, CC(CA_RASC, 14, 12, CA_ZERO) | 3u << 16 | eq << 18 | mode << 20);
            gx_bp(BP_COMBINER + 1, AC(AA_RASA, 6, 2, AA_ZERO) | 3u << 16 | eq << 18 | mode << 20);
            if (!eq) {
                /* r crece a la derecha, g hacia arriba, b y a en diagonal;
                 * pasan por 128 en el medio */
                quad4((float)x, 4, 64, 64, 0x00FF0000u, 0xFFFF8080u, 0xFF00FFFFu, 0x00008080u);
            } else {
                /* Igual a K0 en todo, en g y b, en r y g, y en b */
                static const uint32_t flat[4] = { 0x80808080u, 0x80C88080u, 0x80803280u, 0x3C808032u };
                for (uint32_t q = 0; q < 4; q++)
                    quad4((float)(x + 32 * (q & 1u)), (float)(4 + 32 * (q >> 1)), 32, 32, flat[q], flat[q],
                          flat[q], flat[q]);
            }
            snap(x, 4, 64, 64);
        }

    /* Recorte a -1024 sin el recorte a 0-255: C0 = -1024 y -1023 con sesgo
     * -128, y d con signo de 11 bits que da la vuelta */
    genmode(0, 1, 1, 0);
    tev_reg(1, 0, 0x400, 0x401, 0x3FF, 0x400);
    gx_bp(BP_COMBINER, (CC(CA_ZERO, CA_ZERO, CA_ZERO, 2) & ~(1u << 19)) | 2u << 16);
    gx_bp(BP_COMBINER + 1, (AC(AA_ZERO, AA_ZERO, AA_ZERO, 1) & ~(1u << 19)) | 2u << 16);
    quad4(4, 72, 32, 32, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    snap(4, 72, 32, 32);
    gx_bp(BP_COMBINER, (CC(CA_ZERO, CA_ZERO, CA_ZERO, 2) & ~(1u << 19)) | 1u << 16 | 1u << 18);
    quad4(40, 72, 32, 32, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    snap(40, 72, 32, 32);
    wait_gpu("9. comparaciones y recorte");
}

int main(void)
{
    OSReport("gx_tev: TEV\n");
    setup();

    /* Las texturas armadas de las partes 5 y 6 */
    board = alloc(tex_bytes(TF_I8, 32, 32));
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
            put_texel(board, TF_I8, 32, x, y, 1, ((x ^ y) & 4u) ? 0xE0u - 3 * x : 0x20u + 3 * y);
    dc_flush(board, tex_bytes(TF_I8, 32, 32));
    bump = alloc(tex_bytes(TF_IA8, 16, 16));
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++)
            put_texel(bump, TF_IA8, 16, x, y, 2, wave(x, 16) << 8 | wave(y + x / 4, 8));
    dc_flush(bump, tex_bytes(TF_IA8, 16, 16));
    zmap = random_bytes(tex_bytes(TF_RGB5A3, 16, 16));

    random_part(0, "1. combinadores al azar");
    random_part(F_TEX, "2. al azar con texturas");
    random_part(F_TEX | F_IND, "3. al azar con indirectas");
    random_part(F_TEX | F_IND | F_PIX, "4. al azar con alfa, Z, textura de Z y niebla");
    indirect_part();
    ztex_part();
    fog_part();
    alpha_part();
    compare_part();
    OSReport("gx_tev: suma de las lecturas %08x\n", (unsigned)peeks);
    OSReport("gx_tev: %u tokens, %u bytes de texturas; %s\n", (unsigned)token, (unsigned)pool_used,
             fails ? "FALLO" : "OK");
    exit(fails ? 1 : 0);
}
