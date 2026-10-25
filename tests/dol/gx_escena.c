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
 * DOL de prueba "gx_escena" (Fase 2, paso C7): una escena chica como la de un
 * juego, con todo lo de la etapa C junto, en 4 cuadros con la cámara girando:
 *
 *   - perspectiva, Z, una luz con atenuación por distancia (XF);
 *   - un piso con una textura con mipmaps (lineal entre niveles) por la luz;
 *   - un cubo que gira, con textura, luz y una constante sumada en una
 *     segunda etapa del TEV;
 *   - agua: una etapa indirecta (mapa de bump) que mueve la textura, con alfa
 *     y mezcla;
 *   - un espejo: el cubo visto desde atrás en un rincón del EFB, copiado a una
 *     textura RGB565 y pegado en un cuadro de la escena;
 *   - un vidrio transparente con el alfa del vértice;
 *   - niebla exponencial en perspectiva con los parámetros de GXSetFog;
 *   - cada cuadro se copia al XFB borrando el EFB, y la VI lo muestra.
 *
 * Se compara contra el renderizador por software de Dolphin: cada triángulo y
 * cada copia, con lo que queda en la RAM (tests/dol/dolphin/gx_escena.cop.txt).
 *
 * Solo para el DOL: no compila en la PC.
 */
#include <stddef.h>

#include "gxlib.h"
#include "rt.h"
#include "video.h"

#define FIFO_SIZE (64u * 1024u)
#define MAX_WAIT  2000000u
#define FRAMES    4
#define MIRROR    0x00400000u                   /* la copia del espejo, 128 × 128 RGB565 */

static uint8_t fifo[FIFO_SIZE] __attribute__((aligned(32)));
static uint8_t checker[10976] __attribute__((aligned(32)));    /* 64 × 64 RGB565 y 6 mipmaps */
static uint8_t water[32 * 32 * 2] __attribute__((aligned(32)));
static uint8_t bump[16 * 16 * 2] __attribute__((aligned(32)));
static uint32_t token;
static int fails;

enum {
    BP_INDMTX = 0x06, BP_TEVIND = 0x10, BP_SCISSOR_TL = 0x20, BP_SCISSOR_BR = 0x21, BP_IREF = 0x27,
    BP_TEVORDER = 0x28, BP_ZMODE = 0x40, BP_BLEND = 0x41, BP_ZCONTROL = 0x43, BP_COPY_XY = 0x49,
    BP_COPY_WH = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D, BP_COPY_YSCALE = 0x4E, BP_CLEAR_AR = 0x4F,
    BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51, BP_COPY = 0x52, BP_COPY_FILTER0 = 0x53, BP_COPY_FILTER1 = 0x54,
    BP_SCISSOR_OFFSET = 0x59, BP_COMBINER = 0xC0, BP_TEV_RA = 0xE0, BP_TEV_BG = 0xE1, BP_FOG_A = 0xEE,
    BP_FOG_BMAG = 0xEF, BP_FOG_BSHIFT = 0xF0, BP_FOG_C = 0xF1, BP_FOG_COLOR = 0xF2, BP_ALPHATEST = 0xF3,
    BP_KSEL = 0xF6,
};
enum { TF_IA8 = 3, TF_RGB565 = 4 };
enum { CA_CPREV = 0, CA_TEXC = 8, CA_RASC = 10, CA_KONST = 14, CA_ZERO = 15 };
enum { AA_APREV = 0, AA_TEXA = 4, AA_RASA = 5, AA_KONST = 6, AA_ZERO = 7 };
#define ZMODE(func, update) (1u | (uint32_t)(func) << 1 | (uint32_t)(update) << 4)
#define Z_LEQUAL 3u
#define B_COLOR_ALPHA (1u << 3 | 1u << 4)
#define B_ALPHA_BLEND (1u | 5u << 5 | 4u << 8 | B_COLOR_ALPHA)
#define CC(a, b, c, d) ((uint32_t)(a) << 12 | (uint32_t)(b) << 8 | (uint32_t)(c) << 4 | (uint32_t)(d) | 1u << 19)
#define AC(a, b, c, d) ((uint32_t)(a) << 13 | (uint32_t)(b) << 10 | (uint32_t)(c) << 7 | (uint32_t)(d) << 4 | 1u << 19)
#define ORDER(map, coord, en, chan) ((uint32_t)(map) | (uint32_t)(coord) << 3 | (uint32_t)(en) << 6 | (uint32_t)(chan) << 7)
#define MODE0(ws, wt, mag, mip, min) ((uint32_t)(ws) | (uint32_t)(wt) << 2 | (uint32_t)(mag) << 4 | \
                                      (uint32_t)(mip) << 5 | (uint32_t)(min) << 7)
#define SKY 0x6090D0FFu

/* Ángulos de 0, 8, 16 y 24 grados: coseno y seno */
static const float trig[FRAMES][2] = {
    { 1.0f, 0.0f }, { 0.990268069f, 0.139173101f }, { 0.961261696f, 0.275637356f },
    { 0.913545458f, 0.406736643f },
};

/* Perspectiva de 60 grados, de 1 a 100 (4:3 y cuadrada) */
static const float persp43[6] = { 1.2990381f, 0, 1.7320508f, 0, -0.01010101f, -1.01010101f };
static const float persp11[6] = { 1.7320508f, 0, 1.7320508f, 0, -0.01010101f, -1.01010101f };
static const float viewport_main[6] = { 320.0f, -240.0f, 16777215.0f, 662.0f, 582.0f, 16777215.0f };
static const float viewport_mirror[6] = { 64.0f, -64.0f, 16777215.0f, 406.0f, 406.0f, 16777215.0f };

/* ---- utilidades -------------------------------------------------------------------- */

static void wait_gpu(const char *what)
{
    token++;
    gx_bp(BP_TOKEN, token);
    gx_flush();
    if (!gx_wait_idle(MAX_WAIT)) {
        OSReport("gx_escena: %s: la GPU no vacio el FIFO\n", what);
        fails++;
        return;
    }
    uint32_t n = 0;
    while (PE_REG(PE_TOKEN) != (uint16_t)token && n < MAX_WAIT)
        n++;
    if (n == MAX_WAIT) {
        OSReport("gx_escena: %s: el token %u no llego\n", what, (unsigned)token);
        fails++;
        return;
    }
    OSReport("gx_escena: %s\n", what);
}

/* Matrices de 3 × 4 (filas x, y, z) */
static void mtx_mul(float o[12], const float a[12], const float b[12])
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            float v = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c];
            if (c == 3)
                v += a[r * 4 + 3];
            o[r * 4 + c] = v;
        }
}

/* Giro en y y después traslado */
static void mtx_ry(float m[12], float c, float s, float tx, float ty, float tz)
{
    const float r[12] = { c, 0, s, tx, 0, 1, 0, ty, -s, 0, c, tz };
    for (int i = 0; i < 12; i++)
        m[i] = r[i];
}

/* La matriz de posición 0 y la de normales (la parte de giro) */
static void load_mv(const float m[12])
{
    const float n[9] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
    gx_pos_matrix(0, m);
    gx_normal_matrix(0, n);
}

static void v(float x, float y, float z, float nx, float ny, float nz, uint32_t c, float s, float t)
{
    gx_f32(x);
    gx_f32(y);
    gx_f32(z);
    gx_f32(nx);
    gx_f32(ny);
    gx_f32(nz);
    gx_u32(c);
    gx_f32(s);
    gx_f32(t);
}

static void genmode(uint32_t texgens, uint32_t stages, uint32_t ind)
{
    gx_bp(BP_GENMODE, texgens | 1u << 4 | (stages - 1) << 10 | ind << 16);
    gx_xf1(XF_NUMTEXGEN, texgens);
    gx_xf1(XF_NUMCHAN, 1);
}

static void tex_unit(uint32_t map, uint32_t mode0, uint32_t maxlod, uint32_t w, uint32_t h, uint32_t fmt,
                     uint32_t phys)
{
    const uint32_t r = 0x80u | (map & 3u) | (map & 4u) << 3;
    gx_bp(r + 0x00, mode0);
    gx_bp(r + 0x04, maxlod << 8);
    gx_bp(r + 0x08, (w - 1) | (h - 1) << 10 | fmt << 20);
    gx_bp(r + 0x0C, 0);
    gx_bp(r + 0x14, phys >> 5);
}

/* Un texel de 16 bits en bloques de 4 × 4 */
static void put16(uint8_t *t, uint32_t w, uint32_t x, uint32_t y, uint32_t v16)
{
    const uint32_t nbx = (w + 3) / 4;
    uint8_t *p = t + (((y / 4) * nbx + x / 4) * 16 + (y % 4) * 4 + x % 4) * 2;
    p[0] = (uint8_t)(v16 >> 8);
    p[1] = (uint8_t)v16;
}

static uint32_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
}

static uint32_t wave(uint32_t x, uint32_t p)
{
    const uint32_t k = (2 * 255 * (x % p)) / p;
    return k <= 255 ? k : 510 - k;
}

/* Fog: el float de 11 bits de mantisa de los registros */
static uint32_t fog_float(float f)
{
    const uint32_t u = f32_bits(f);
    return (u >> 31) << 19 | ((u >> 23) & 0xFFu) << 11 | ((u >> 12) & 0x7FFu);
}

/* GXSetFog: exponencial en perspectiva entre startz y endz con near y far */
static void set_fog(float startz, float endz, float nearz, float farz, uint32_t color)
{
    const float a = (farz * nearz) / ((farz - nearz) * (endz - startz));
    const float b = farz / (farz - nearz);
    const float c = (startz - nearz) / (endz - startz);
    float mant = b;
    int exp = 1;
    while (mant > 1.0f) {
        mant /= 2;
        exp++;
    }
    while (mant > 0 && mant < 0.5f) {
        mant *= 2;
        exp--;
    }
    gx_bp(BP_FOG_A, fog_float(a / (float)(1u << exp)));
    gx_bp(BP_FOG_BMAG, (uint32_t)(mant * 8388638.0f));
    gx_bp(BP_FOG_BSHIFT, (uint32_t)exp);
    gx_bp(BP_FOG_C, fog_float(c) | 4u << 21);
    gx_bp(BP_FOG_COLOR, color >> 8);
}

/* ---- texturas ----------------------------------------------------------------------- */

static void make_textures(void)
{
    /* Tablero con un color por nivel, para ver el paso entre mipmaps */
    static const uint32_t tint[7][3] = { { 240, 230, 200 }, { 230, 180, 160 }, { 200, 220, 150 }, { 150, 200, 220 },
                                         { 200, 150, 220 }, { 220, 200, 120 }, { 180, 180, 180 } };
    uint32_t off = 0;
    for (uint32_t lvl = 0, s = 64; lvl < 7; lvl++, s /= 2) {
        const uint32_t cell = s >= 8 ? s / 8 : 1;
        for (uint32_t y = 0; y < s; y++)
            for (uint32_t x = 0; x < s; x++) {
                const int dark = (int)(((x / cell) ^ (y / cell)) & 1u);
                const uint32_t k = dark ? 3 : 4;
                put16(checker + off, s, x, y,
                      rgb565(tint[lvl][0] * k / 4, tint[lvl][1] * k / 4, tint[lvl][2] * k / 4));
            }
        const uint32_t bytes = s * s * 2;
        off += bytes < 32 ? 32 : bytes;
    }
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
            put16(water, 32, x, y, rgb565(30 + wave(x + y, 16) / 4, 90 + wave(y, 8) / 3, 200));
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++)
            put16(bump, 16, x, y, wave(x, 16) << 8 | wave(y + x / 4, 8));
    dc_flush(checker, sizeof checker);
    dc_flush(water, sizeof water);
    dc_flush(bump, sizeof bump);
}

/* ---- estado ------------------------------------------------------------------------- */

static void setup(void)
{
    gx_init_fifo(fifo, FIFO_SIZE, FIFO_SIZE - 16384, FIFO_SIZE / 2, CP_CR_READ | CP_CR_LINK);
    gx_xf1(XF_DUALTEX, 0);
    static const float tex_ident[8] = { 1, 0, 0, 0, 0, 1, 0, 0 };
    static const float tex_bump[8] = { 2, 0, 0, 0, 0, 2, 0, 0 };
    gx_xf_floats(30 * 4, 8, tex_ident);
    gx_xf_floats(33 * 4, 8, tex_bump);
    gx_matindex(0 | 30u << 6 | 33u << 12, 0);
    gx_xf1(XF_TEXMTXINFO, TG_ROW(5));
    gx_xf1(XF_TEXMTXINFO + 1, TG_ROW(5));
    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_NRM(0, FMT_F32) | VAT_COL0(COL_8888) | VAT_TEX0(1, FMT_F32, 0), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_NRM(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), VCD_TEX(0, VTX_DIRECT), 0);

    /* Una luz arriba y adelante (en el espacio de la cámara), sin ángulo, que
     * se apaga con la distancia */
    static const float cosatt[3] = { 1, 0, 0 }, distatt[3] = { 1, 0.04f, 0 };
    static const float lpos[3] = { 3, 8, -6 }, ldir[3] = { 0, -1, 0 };
    gx_light(0, 0xFFF0E0FFu, cosatt, distatt, lpos, ldir);
    gx_xf1(XF_AMB0, 0x404850FFu);
    gx_xf1(XF_MAT0, 0xFFFFFFFFu);
    gx_xf1(XF_ALPHA0, CH_MATSRC_VTX);

    gx_bp(BP_SCISSOR_OFFSET, 171 | 171u << 10);
    gx_bp(BP_KSEL, 1u << 2 | 12u << 4 | 28u << 9 | 12u << 14);    /* swap normal, K0 */
    gx_bp(BP_KSEL + 1, 2u | 3u << 2);
    gx_bp(BP_TEV_RA, 40 | 0x99u << 12 | 1u << 23);                /* K0: (40, 10, 30), alfa 0x99 */
    gx_bp(BP_TEV_BG, 30 | 10u << 12 | 1u << 23);
    gx_bp(BP_ALPHATEST, 7u << 16 | 7u << 19);
    gx_bp(BP_ZCONTROL, 0);                      /* RGB8 */
    gx_bp(BP_COPY_FILTER0, 0 | 0u << 6 | 21u << 12 | 22u << 18);
    gx_bp(BP_COPY_FILTER1, 21);
    gx_bp(BP_COPY_YSCALE, 256);
    /* El agua: indirecta 0 con el mapa 1 y la coordenada 1, matriz chica */
    gx_bp(BP_IREF, 1 | 1u << 3);
    gx_bp(BP_INDMTX, 60 | 0u << 11 | 1u << 22);
    gx_bp(BP_INDMTX + 1, 0 | 60u << 11 | 0u << 22);
    gx_bp(BP_INDMTX + 2, 0 | 0u << 11 | 1u << 22);                /* escala 1 + 0 + 16: 17 */
    set_fog(14.0f, 60.0f, 1.0f, 100.0f, SKY);

    tex_unit(0, MODE0(1, 1, 1, 2, 1), 6 * 16, 64, 64, TF_RGB565, gx_phys(checker));
    tex_unit(1, MODE0(1, 1, 0, 0, 0), 0, 16, 16, TF_IA8, gx_phys(bump));
    tex_unit(2, MODE0(1, 1, 1, 0, 1), 0, 32, 32, TF_RGB565, gx_phys(water));
    tex_unit(3, MODE0(0, 0, 1, 0, 1), 0, 128, 128, TF_RGB565, MIRROR);
    gx_bp(BP_SU_SSIZE + 2, 15);
    gx_bp(BP_SU_TSIZE + 2, 15);
    wait_gpu("estado inicial");
}

/* Lo que comparten los objetos con luz y textura: la etapa 0 es tex × ras */
/* La escala de la coordenada 0 (BP 0x30-0x31): el tamaño de la textura */
static void su0(uint32_t size)
{
    gx_bp(BP_SU_SSIZE, size - 1);
    gx_bp(BP_SU_TSIZE, size - 1);
}

static void lit_textured(uint32_t map, int add_konst)
{
    su0(64);
    gx_xf1(XF_COLOR0, CH_MATSRC_VTX | CH_LIGHT | CH_MASK(1) | CH_DIFF(2) | CH_ATTN(3));
    genmode(1, add_konst ? 2 : 1, 0);
    gx_bp(BP_TEVORDER, ORDER(map, 0, 1, 0) | ORDER(0, 0, 0, 7) << 12);
    gx_bp(BP_COMBINER, CC(CA_ZERO, CA_TEXC, CA_RASC, CA_ZERO));
    gx_bp(BP_COMBINER + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_RASA));
    gx_bp(BP_COMBINER + 2, CC(CA_KONST, CA_ZERO, CA_ZERO, CA_CPREV));
    gx_bp(BP_COMBINER + 3, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_APREV));
    gx_bp(BP_TEVIND, 0);
}

static void unlit(void)
{
    gx_xf1(XF_COLOR0, CH_MATSRC_VTX);
}

/* ---- objetos ------------------------------------------------------------------------ */

static void floor_grid(void)
{
    lit_textured(0, 0);
    gx_bp(BP_ZMODE, ZMODE(Z_LEQUAL, 1));
    gx_bp(BP_BLEND, B_COLOR_ALPHA);
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 5; i++) {
            const float x = -12.0f + 4.8f * (float)i, z = -12.0f + 4.8f * (float)j;
            gx_begin(GX_QUADS, 0, 4);
            v(x, 0, z, 0, 1, 0, 0xFFFFFFFFu, (float)i * 2, (float)j * 2);
            v(x + 4.8f, 0, z, 0, 1, 0, 0xFFFFFFFFu, (float)i * 2 + 2, (float)j * 2);
            v(x + 4.8f, 0, z + 4.8f, 0, 1, 0, 0xFFFFFFFFu, (float)i * 2 + 2, (float)j * 2 + 2);
            v(x, 0, z + 4.8f, 0, 1, 0, 0xFFFFFFFFu, (float)i * 2, (float)j * 2 + 2);
        }
}

/* Un cubo de lado 2 con centro en el origen; un color por cara */
static void cube(void)
{
    static const float n[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    static const uint32_t col[6] = { 0xFFC0C0FFu, 0xC0FFC0FFu, 0xC0C0FFFFu, 0xFFFFC0FFu, 0xFFC0FFFFu, 0xC0FFFFFFu };
    lit_textured(0, 1);
    gx_bp(BP_ZMODE, ZMODE(Z_LEQUAL, 1));
    gx_bp(BP_BLEND, B_COLOR_ALPHA);
    for (int f = 0; f < 6; f++) {
        /* Dos ejes del plano de la cara: u y w = n × u */
        const float *nn = n[f];
        float u[3] = { nn[1] != 0 ? 1.0f : 0.0f, 0, 0 };
        if (nn[1] == 0) {
            u[0] = nn[2];
            u[2] = -nn[0];
        }
        const float w[3] = { nn[1] * u[2] - nn[2] * u[1], nn[2] * u[0] - nn[0] * u[2], nn[0] * u[1] - nn[1] * u[0] };
        static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        gx_begin(GX_QUADS, 0, 4);
        for (int k = 0; k < 4; k++) {
            const float a = corner[k][0], b = corner[k][1];
            v(nn[0] + a * u[0] + b * w[0], nn[1] + a * u[1] + b * w[1], nn[2] + a * u[2] + b * w[2], nn[0], nn[1],
              nn[2], col[f], (a + 1) * 0.5f, (b + 1) * 0.5f);
        }
    }
}

static void pond(void)
{
    unlit();
    su0(32);
    genmode(2, 1, 1);
    gx_bp(BP_TEVORDER, ORDER(2, 0, 1, 0));
    gx_bp(BP_TEVIND, 0 | 0u << 2 | 7u << 4 | 1u << 9);   /* ITF_8, sesgo STU, matriz 0 */
    gx_bp(BP_COMBINER, CC(CA_ZERO, CA_TEXC, CA_RASC, CA_ZERO));
    gx_bp(BP_COMBINER + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_KONST));
    gx_bp(BP_ZMODE, ZMODE(Z_LEQUAL, 0));
    gx_bp(BP_BLEND, B_ALPHA_BLEND);
    gx_begin(GX_QUADS, 0, 4);
    v(4, 0.02f, -2, 0, 1, 0, 0xFFFFFFFFu, 0, 0);
    v(9, 0.02f, -2, 0, 1, 0, 0xFFFFFFFFu, 2, 0);
    v(9, 0.02f, 3, 0, 1, 0, 0xFFFFFFFFu, 2, 2);
    v(4, 0.02f, 3, 0, 1, 0, 0xFFFFFFFFu, 0, 2);
    gx_bp(BP_TEVIND, 0);
}

static void mirror_quad(void)
{
    unlit();
    su0(128);
    genmode(1, 1, 0);
    gx_bp(BP_TEVORDER, ORDER(3, 0, 1, 0));
    gx_bp(BP_COMBINER, CC(CA_ZERO, CA_TEXC, CA_RASC, CA_ZERO));
    gx_bp(BP_COMBINER + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_RASA));
    gx_bp(BP_ZMODE, ZMODE(Z_LEQUAL, 1));
    gx_bp(BP_BLEND, B_COLOR_ALPHA);
    gx_begin(GX_QUADS, 0, 4);
    v(-7, 4, -6, 0, 0, 1, 0xE0E0F0FFu, 0, 0);
    v(-3, 4, -6, 0, 0, 1, 0xE0E0F0FFu, 1, 0);
    v(-3, 0, -6, 0, 0, 1, 0xE0E0F0FFu, 1, 1);
    v(-7, 0, -6, 0, 0, 1, 0xE0E0F0FFu, 0, 1);
}

static void glass(void)
{
    unlit();
    genmode(1, 1, 0);
    gx_bp(BP_TEVORDER, ORDER(0, 0, 0, 0));
    gx_bp(BP_COMBINER, CC(CA_ZERO, CA_ZERO, CA_ZERO, CA_RASC));
    gx_bp(BP_COMBINER + 1, AC(AA_ZERO, AA_ZERO, AA_ZERO, AA_RASA));
    gx_bp(BP_ZMODE, ZMODE(Z_LEQUAL, 0));
    gx_bp(BP_BLEND, B_ALPHA_BLEND);
    gx_begin(GX_QUADS, 0, 4);
    v(-1, 0, 4, 0, 0, 1, 0xFF902060u, 0, 0);
    v(3, 0, 4, 0, 0, 1, 0xFFC04090u, 0, 0);
    v(3, 2.5f, 4, 0, 0, 1, 0xFFE060A0u, 0, 0);
    v(-1, 2.5f, 4, 0, 0, 1, 0xFF803050u, 0, 0);
}

/* ---- un cuadro ---------------------------------------------------------------------- */

static void copy_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    gx_bp(BP_COPY_XY, x | y << 10);
    gx_bp(BP_COPY_WH, (w - 1) | (h - 1) << 10);
}

static void clear_color(uint32_t rgba)
{
    gx_bp(BP_CLEAR_AR, (rgba & 0xFFu) << 8 | rgba >> 24);
    gx_bp(BP_CLEAR_GB, (rgba >> 8) & 0xFFFFu);
    gx_bp(BP_CLEAR_Z, 0xFFFFFF);
}

static void scissor(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    gx_bp(BP_SCISSOR_TL, (y0 + 342) | (x0 + 342) << 12);
    gx_bp(BP_SCISSOR_BR, (y1 + 342) | (x1 + 342) << 12);
}

static void frame(int f)
{
    float view[12], world[12], mv[12];
    const float c = trig[f][0], s = trig[f][1];
    const float c2 = c * c - s * s, s2 = 2 * c * s;   /* el cubo gira el doble */

    /* El espejo: el cubo desde atrás en el rincón de 128 × 128, a textura */
    gx_projection(persp11, 0);
    gx_xf_floats(XF_VIEWPORT, 6, viewport_mirror);
    scissor(0, 0, 127, 127);
    mtx_ry(view, -1, 0, 0, -1.6f, -6);
    mtx_ry(world, c2, -s2, 0, 1.6f, 0);
    mtx_mul(mv, view, world);
    load_mv(mv);
    cube();
    copy_rect(0, 0, 128, 128);
    gx_bp(BP_COPY_DST, MIRROR >> 5);
    gx_bp(BP_COPY_STRIDE, 32 * 32 >> 5);        /* 32 bloques de 32 bytes por fila */
    gx_bp(BP_COPY, 8u << 3 | 1u << 11);         /* RGB565 (tp 8: 8 / 2), borrando */

    /* La escena */
    gx_projection(persp43, 0);
    gx_xf_floats(XF_VIEWPORT, 6, viewport_main);
    scissor(0, 0, 639, 479);
    /* La cámara: gira en y, se aleja y mira 15 grados hacia abajo */
    static const float pitch[12] = { 1, 0, 0, 0, 0, 0.965925826f, -0.258819045f, 0, 0, 0.258819045f, 0.965925826f, 0 };
    float yaw[12];
    mtx_ry(yaw, c, s, 0, -2.5f, -10.0f);
    mtx_mul(view, pitch, yaw);
    load_mv(view);
    floor_grid();
    mtx_ry(world, c2, -s2, 0, 1.6f, 0);
    mtx_mul(mv, view, world);
    load_mv(mv);
    cube();
    load_mv(view);
    mirror_quad();
    pond();
    glass();

    /* Al XFB, borrando color y Z para el cuadro siguiente */
    gx_bp(BP_ZMODE, ZMODE(7, 1));
    gx_bp(BP_BLEND, B_COLOR_ALPHA);
    copy_rect(0, 0, 640, 480);
    gx_bp(BP_COPY_DST, XFB_PHYS >> 5);
    gx_bp(BP_COPY_STRIDE, 1280 >> 5);
    gx_bp(BP_COPY, 1u << 14 | 1u | 2u | 1u << 11);
}

int main(void)
{
    OSReport("gx_escena: una escena\n");
    make_textures();
    setup();
    /* El primer borrado */
    clear_color(SKY);
    gx_bp(BP_ZMODE, ZMODE(7, 1));
    gx_bp(BP_BLEND, B_COLOR_ALPHA);
    scissor(0, 0, 639, 527);
    copy_rect(0, 0, 640, 528);
    gx_bp(BP_COPY_DST, 0);
    gx_bp(BP_COPY_STRIDE, 0);
    gx_bp(BP_COPY, 1u << 11);
    video_init_480i();
    static const char *const names[FRAMES] = { "cuadro 1", "cuadro 2", "cuadro 3", "cuadro 4" };
    for (int f = 0; f < FRAMES; f++) {
        frame(f);
        wait_gpu(names[f]);
        video_wait_frame();
        video_wait_frame();
    }
    OSReport("gx_escena: %u tokens; %s\n", (unsigned)token, fails ? "FALLO" : "OK");
    exit(fails ? 1 : 0);
}
