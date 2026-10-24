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
 * Fase 2, etapa B: el XF (src/gx/xform.c). Los valores esperados son cuentas
 * a mano con números exactos, siguiendo TransformUnit.cpp de Dolphin
 * cf5cb7ef; los bits de los casos generales los compara el DOL gx_tri.
 */
#include <math.h>
#include <string.h>

#include "gx/xform.h"
#include "test.h"

/* Registros (desde 0x1000) y memoria de XF */
enum {
    R_NUMCHAN = 0x09, R_AMB0 = 0x0A, R_MAT0 = 0x0C, R_COLOR0 = 0x0E, R_ALPHA0 = 0x10,
    R_DUALTEX = 0x12, R_PROJ = 0x20, R_PROJ_TYPE = 0x26, R_NUMTEXGEN = 0x3F, R_TEXMTXINFO = 0x40,
    R_POSTMTXINFO = 0x50,
};
enum { M_NORMAL = 0x400, M_POST = 0x500, M_LIGHTS = 0x600 };

/* LitChannel */
#define CH_MATSRC_VTX     1u
#define CH_LIGHT          2u
#define CH_MASK(m)        (((uint32_t)(m) & 15u) << 2 | ((uint32_t)(m) >> 4 & 15u) << 11)
#define CH_AMBSRC_VTX     (1u << 6)
#define CH_DIFF(d)        ((uint32_t)(d) << 7)
#define CH_ATTN(a)        ((uint32_t)(a) << 9)
enum { D_NONE = 0, D_SIGN = 1, D_CLAMP = 2 };
enum { A_SPEC = 1, A_DIR = 2, A_SPOT = 3 };

/* TexMtxInfo */
#define TI_STQ            2u
#define TI_ABC1           4u
#define TI_TYPE(t)        ((uint32_t)(t) << 4)
#define TI_ROW(r)         ((uint32_t)(r) << 7)
#define TI_EMB(src, l)    ((uint32_t)(src) << 12 | (uint32_t)(l) << 15)
enum { ROW_GEOM = 0, ROW_NORMAL = 1, ROW_BT = 3, ROW_BB = 4, ROW_TEX0 = 5 };

static gx_xf_snap_t xf;
static gx_vtx_in_t in[4];
static gx_vtx_out_t out[4];

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static void setf(uint32_t at, float f)
{
    xf.mem[at] = f;
}

static void setu(uint32_t at, uint32_t u)
{
    memcpy(&xf.mem[at], &u, sizeof u);
}

static void set_reg_f(uint32_t reg, float f)
{
    xf.reg[reg] = fbits(f);
}

/* Matriz de 3 × 4 (o 2 × 4, o 3 × 3) por filas desde at */
static void set_mat(uint32_t at, const float *m, int n)
{
    for (int k = 0; k < n; k++)
        setf(at + (uint32_t)k, m[k]);
}

static const float ident34[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
static const float ident33[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

static void reset(void)
{
    memset(&xf, 0, sizeof xf);
    memset(in, 0, sizeof in);
    memset(out, 0xCD, sizeof out);
    for (int k = 0; k < 8; k++)
        xf.tex_scale[k][0] = xf.tex_scale[k][1] = 1.0f;
    set_mat(0, ident34, 12);
    set_mat(M_NORMAL, ident33, 9);
    set_reg_f(R_PROJ + 0, 1.0f);
    set_reg_f(R_PROJ + 2, 1.0f);
    set_reg_f(R_PROJ + 4, 1.0f);
    xf.reg[R_PROJ_TYPE] = 1;                    /* ortográfica */
    for (int i = 0; i < 4; i++) {
        memset(in[i].col, 255, sizeof in[i].col);
        in[i].nrm[0][2] = 1.0f;
    }
}

static void set_light(uint32_t i, uint32_t color, const float cosatt[3], const float distatt[3],
                      const float pos[3], const float dir[3])
{
    const uint32_t b = M_LIGHTS + 16u * i;
    setu(b + 3, color);
    for (int k = 0; k < 3; k++) {
        setf(b + 4 + (uint32_t)k, cosatt[k]);
        setf(b + 7 + (uint32_t)k, distatt[k]);
        setf(b + 10 + (uint32_t)k, pos[k]);
        setf(b + 13 + (uint32_t)k, dir[k]);
    }
}

static uint32_t rgba(const uint8_t c[4])
{
    return (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3];
}

/* ---- posición ---------------------------------------------------------------------- */

static void test_xform_position(void)
{
    reset();
    /* Matriz 34: fila 0 en 34 × 4 = 136 */
    const float m[12] = { 2, 0, 0, 1, 0, 3, 0, 2, 0, 0, 1, -5 };
    set_mat(34 * 4, m, 12);
    in[0].posmtx = 34;
    in[0].pos[0] = 1;
    in[0].pos[1] = 2;
    in[0].pos[2] = 3;
    set_reg_f(R_PROJ + 0, 1.5f);
    set_reg_f(R_PROJ + 1, 0.5f);
    set_reg_f(R_PROJ + 2, 2.0f);
    set_reg_f(R_PROJ + 3, 0.25f);
    set_reg_f(R_PROJ + 4, -1.0f);
    set_reg_f(R_PROJ + 5, -1.0f);

    /* Ortográfica */
    xf.reg[R_PROJ_TYPE] = 1;
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(fbits(out[0].mv[0]), fbits(3.0f));
    CHECK_EQ_U64(fbits(out[0].mv[1]), fbits(8.0f));
    CHECK_EQ_U64(fbits(out[0].mv[2]), fbits(-2.0f));
    CHECK_EQ_U64(fbits(out[0].proj[0]), fbits(5.0f));
    CHECK_EQ_U64(fbits(out[0].proj[1]), fbits(16.25f));
    CHECK_EQ_U64(fbits(out[0].proj[2]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].proj[3]), fbits(1.0f));

    /* Perspectiva: z por (1 - 1e-7) en float, w = -z en la vista */
    xf.reg[R_PROJ_TYPE] = 0;
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(fbits(out[0].proj[0]), fbits(3.5f));
    CHECK_EQ_U64(fbits(out[0].proj[1]), fbits(15.5f));
    CHECK_EQ_U64(fbits(out[0].proj[2]), 0x3F7FFFFEu);
    CHECK_EQ_U64(fbits(out[0].proj[3]), fbits(2.0f));
}

/* La matriz normal es la de posmtx & 31; solo la normal se normaliza */
static void test_xform_normals(void)
{
    reset();
    const float n[9] = { 0, 2, 0, 2, 0, 0, 0, 0, 2 };
    set_mat(M_NORMAL + 2 * 3, n, 9);
    in[0].posmtx = 34;
    in[0].nrm[0][0] = 3;
    in[0].nrm[0][1] = 0;
    in[0].nrm[0][2] = 4;
    in[0].nrm[1][0] = 1;
    in[0].nrm[2][2] = -1;
    gx_xform(&xf, in, 1, out);
    const float six = 6.0f, eight = 8.0f, ten = 10.0f;
    CHECK_EQ_U64(fbits(out[0].nrm[0][0]), 0);
    CHECK_EQ_U64(fbits(out[0].nrm[0][1]), fbits(six / ten));
    CHECK_EQ_U64(fbits(out[0].nrm[0][2]), fbits(eight / ten));
    CHECK_EQ_U64(fbits(out[0].nrm[1][1]), fbits(2.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[2][2]), fbits(-2.0f));

    /* Normal cero: 0/0 da NaN */
    in[0].nrm[0][0] = in[0].nrm[0][1] = in[0].nrm[0][2] = 0;
    gx_xform(&xf, in, 1, out);
    CHECK(isnan(out[0].nrm[0][0]) && isnan(out[0].nrm[0][2]));
}

/* ---- color ---------------------------------------------------------------------------- */

static void test_xform_color_no_lighting(void)
{
    reset();
    xf.reg[R_COLOR0] = CH_MATSRC_VTX;
    xf.reg[R_ALPHA0] = 0;
    xf.reg[R_MAT0] = 0x11223344u;
    xf.reg[R_COLOR0 + 1] = 0;
    xf.reg[R_ALPHA0 + 1] = CH_MATSRC_VTX;
    xf.reg[R_MAT0 + 1] = 0x55667788u;
    memcpy(in[0].col[0], (const uint8_t[]){ 1, 2, 3, 4 }, 4);
    memcpy(in[0].col[1], (const uint8_t[]){ 5, 6, 7, 8 }, 4);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(rgba(out[0].col[0]), 0x01020344u);
    CHECK_EQ_U64(rgba(out[0].col[1]), 0x55667708u);
}

static void test_xform_lights_diffuse(void)
{
    reset();
    const float z3[3] = { 0, 0, 0 }, up[3] = { 0, 0, 10 };
    set_light(0, 0x80400040u, z3, z3, up, z3);
    set_light(5, 0x00002010u, z3, z3, up, z3);     /* la máscara alta (bits 11-14) */
    xf.reg[R_MAT0] = 0xFFFFFFFFu;
    xf.reg[R_AMB0] = 0x20202000u;
    xf.reg[R_COLOR0] = CH_LIGHT | CH_MASK(0x21) | CH_DIFF(D_CLAMP) | CH_ATTN(A_DIR);
    xf.reg[R_ALPHA0] = CH_LIGHT | CH_MASK(0x01) | CH_DIFF(D_NONE);
    gx_xform(&xf, in, 1, out);
    /* luz = 32 + (128, 64, 0) + (0, 0, 32); (255 × (l + l >> 7)) >> 8 */
    CHECK_EQ_U64(rgba(out[0].col[0]), (uint32_t)160 << 24 | 95u << 16 | 63u << 8 | 63u);

    /* Signo con la normal de espaldas: la luz resta y se recorta en 0 */
    in[0].nrm[0][2] = -1;
    xf.reg[R_COLOR0] = CH_LIGHT | CH_MASK(0x01) | CH_DIFF(D_SIGN);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(out[0].col[0][0], 0);              /* 32 - 128 */
    CHECK_EQ_U64(out[0].col[0][1], 0);              /* 32 - 64 */
    CHECK_EQ_U64(out[0].col[0][2], 31);             /* 32 - 0 */

    /* Ambiente y material del vértice */
    in[0].nrm[0][2] = 1;
    memcpy(in[0].col[0], (const uint8_t[]){ 128, 64, 255, 128 }, 4);
    xf.reg[R_COLOR0] = CH_MATSRC_VTX | CH_LIGHT | CH_AMBSRC_VTX | CH_DIFF(D_CLAMP);
    xf.reg[R_ALPHA0] = CH_MATSRC_VTX | CH_LIGHT | CH_AMBSRC_VTX;
    gx_xform(&xf, in, 1, out);
    /* sin luces: luz = ambiente = color; (c × (c + c >> 7)) >> 8 */
    CHECK_EQ_U64(rgba(out[0].col[0]), (uint32_t)64 << 24 | 16u << 16 | 255u << 8 | 64u);
}

/* La luz en el mismo punto que el vértice: la dirección es 0/0 */
static void test_xform_light_nan(void)
{
    reset();
    const float z3[3] = { 0, 0, 0 };
    set_light(0, 0x80808080u, z3, z3, z3, z3);
    xf.reg[R_MAT0] = 0xFFFFFFFFu;
    xf.reg[R_AMB0] = 0x20202020u;
    /* Recortada: max(0, NaN) = 0, la luz no suma */
    xf.reg[R_COLOR0] = CH_LIGHT | CH_MASK(1) | CH_DIFF(D_CLAMP);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(out[0].col[0][0], 31);
    /* Con signo: NaN, que al pasar a int da INT_MIN, y el recorte da 0 */
    xf.reg[R_COLOR0] = CH_LIGHT | CH_MASK(1) | CH_DIFF(D_SIGN);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(out[0].col[0][0], 0);
}

static void test_xform_light_attn(void)
{
    reset();
    xf.reg[R_MAT0] = 0xFFFFFFFFu;
    xf.reg[R_AMB0] = 0x20202020u;

    /* Spot: dist = 10, attn = (0.5 + 0.25 + 0.25) / (2 + 0 + 0) = 0.5 */
    const float up[3] = { 0, 0, 10 }, dir[3] = { 0, 0, 1 };
    set_light(0, 0x80000000u, (const float[]){ 0.5f, 0.25f, 0.25f }, (const float[]){ 2, 0, 0 }, up, dir);
    xf.reg[R_COLOR0] = CH_LIGHT | CH_MASK(1) | CH_DIFF(D_CLAMP) | CH_ATTN(A_SPOT);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(out[0].col[0][0], 95);             /* 32 + 64 */

    /* Spot con atenuación por distancia 0: SafeDivide da 1 */
    set_light(0, 0x80000000u, (const float[]){ 1, 0, 0 }, (const float[]){ 0, 0, 0 }, up, dir);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(out[0].col[0][0], 160);            /* 32 + 128 */

    /* Especular: attn = max(0, dir · n) = 0.5; (1, .5, .25) · (0, 2, 0) = 1;
     * con difusa, distatt (0, 0, 2) se normaliza: 0.25; 1 / 0.25 = 4 */
    set_light(0, 0x10000000u, (const float[]){ 0, 2, 0 }, (const float[]){ 0, 0, 2 }, up,
              (const float[]){ 0, 0, 0.5f });
    xf.reg[R_COLOR0] = CH_LIGHT | CH_MASK(1) | CH_DIFF(D_CLAMP) | CH_ATTN(A_SPEC);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(out[0].col[0][0], 95);             /* 32 + 16 × 4 */
    /* Sin difusa no se normaliza: 1 / 0.5 = 2 */
    xf.reg[R_COLOR0] = CH_LIGHT | CH_MASK(1) | CH_DIFF(D_NONE) | CH_ATTN(A_SPEC);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(out[0].col[0][0], 63);             /* 32 + 16 × 2 */
    /* La luz detrás de la normal: attn 0, y SafeDivide(0, 0) da 0 */
    in[0].nrm[0][2] = -1;
    xf.reg[R_COLOR0] = CH_LIGHT | CH_MASK(1) | CH_DIFF(D_CLAMP) | CH_ATTN(A_SPEC);
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(out[0].col[0][0], 31);
}

/* ---- coordenadas de textura ---------------------------------------------------------- */

static void test_xform_texgen_regular(void)
{
    reset();
    xf.reg[R_NUMTEXGEN] = 4;
    in[0].pos[0] = 1;
    in[0].pos[1] = 2;
    in[0].pos[2] = 3;
    in[0].tex[0][0] = 1;
    in[0].tex[0][1] = 2;

    /* 0: Tex0, ST, AB11 con la matriz 10: (2s + m2 + m3, 4t + m6 + m7, 1) */
    set_mat(10 * 4, (const float[]){ 2, 0, 0.5f, 1, 0, 4, 0.25f, 2 }, 8);
    in[0].texmtx[0] = 10;
    xf.reg[R_TEXMTXINFO + 0] = TI_ROW(ROW_TEX0);
    /* 1: posición, STQ, ABC1 con la matriz 20 */
    set_mat(20 * 4, (const float[]){ 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 0 }, 12);
    in[0].texmtx[1] = 20;
    xf.reg[R_TEXMTXINFO + 1] = TI_STQ | TI_ABC1 | TI_ROW(ROW_GEOM);
    /* 2: q = 0, x e y se dividen por 2 y se recortan */
    set_mat(30 * 4, (const float[]){ 4, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0 }, 12);
    in[0].texmtx[2] = 30;
    xf.reg[R_TEXMTXINFO + 2] = TI_STQ | TI_ABC1 | TI_ROW(ROW_GEOM);
    /* 3: la normal con la identidad */
    in[0].texmtx[3] = 0;
    xf.reg[R_TEXMTXINFO + 3] = TI_STQ | TI_ABC1 | TI_ROW(ROW_NORMAL);
    xf.tex_scale[1][0] = 2;
    xf.tex_scale[1][1] = 4;

    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(fbits(out[0].tex[0][0]), fbits(3.5f));
    CHECK_EQ_U64(fbits(out[0].tex[0][1]), fbits(10.25f));
    CHECK_EQ_U64(fbits(out[0].tex[0][2]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].tex[1][0]), fbits(2.0f));      /* 1 × 2 */
    CHECK_EQ_U64(fbits(out[0].tex[1][1]), fbits(12.0f));     /* 3 × 4 */
    CHECK_EQ_U64(fbits(out[0].tex[1][2]), fbits(6.0f));      /* q sin escala */
    CHECK_EQ_U64(fbits(out[0].tex[2][0]), fbits(1.0f));      /* 4 / 2 recortado */
    CHECK_EQ_U64(fbits(out[0].tex[2][1]), fbits(-1.0f));     /* -2 / 2 */
    CHECK_EQ_U64(fbits(out[0].tex[2][2]), 0);
    CHECK_EQ_U64(fbits(out[0].tex[3][2]), fbits(1.0f));      /* la normal de entrada, sin normalizar */

    /* Un NaN de entrada vale 1 */
    in[0].pos[0] = NAN;
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(fbits(out[0].tex[1][0]), fbits(2.0f));
    in[0].pos[0] = 5;
    in[0].pos[2] = NAN;
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(fbits(out[0].tex[1][2]), fbits(2.0f));
}

static void test_xform_texgen_dual(void)
{
    reset();
    xf.reg[R_NUMTEXGEN] = 2;
    xf.reg[R_DUALTEX] = 1;
    in[0].tex[0][0] = 3;
    in[0].tex[0][1] = 4;
    in[0].tex[1][0] = 3;
    in[0].tex[1][1] = 4;
    xf.reg[R_TEXMTXINFO + 0] = TI_STQ | TI_ROW(ROW_TEX0);
    xf.reg[R_TEXMTXINFO + 1] = TI_STQ | TI_ROW(ROW_TEX0 + 1);
    /* Con la matriz 1 queda (3, 4, 0); q = 0 se arregla después de la matriz final */
    set_mat(1 * 4, (const float[]){ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 }, 12);
    in[0].texmtx[0] = 1;
    in[0].texmtx[1] = 1;
    const float post[12] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 1 };
    set_mat(M_POST + 5 * 4, post, 12);
    xf.reg[R_POSTMTXINFO + 0] = 5;                  /* sin normalizar */
    xf.reg[R_POSTMTXINFO + 1] = 5 | 1u << 8;        /* normalizado: (0.6, 0.8, 0) */
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(fbits(out[0].tex[0][0]), fbits(6.0f));
    CHECK_EQ_U64(fbits(out[0].tex[0][1]), fbits(8.0f));
    CHECK_EQ_U64(fbits(out[0].tex[0][2]), fbits(1.0f));
    const float three = 3.0f, four = 4.0f, five = 5.0f;
    CHECK_EQ_U64(fbits(out[0].tex[1][0]), fbits((three / five) * 2.0f));
    CHECK_EQ_U64(fbits(out[0].tex[1][1]), fbits((four / five) * 2.0f));
    CHECK_EQ_U64(fbits(out[0].tex[1][2]), fbits(1.0f));

    /* Sin la matriz final, q = 0 */
    xf.reg[R_DUALTEX] = 0;
    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(fbits(out[0].tex[0][0]), fbits(1.0f));      /* 3 / 2 recortado */
    CHECK_EQ_U64(fbits(out[0].tex[0][2]), 0);
}

static void test_xform_texgen_emboss_color(void)
{
    reset();
    xf.reg[R_NUMTEXGEN] = 3;
    /* 0: Tex0 con la identidad */
    in[0].tex[0][0] = 0.5f;
    in[0].tex[0][1] = 0.25f;
    xf.reg[R_TEXMTXINFO + 0] = TI_ROW(ROW_TEX0);
    /* 1: emboss desde la 0 con la luz 3, sobre (0, 0, 8) desde el origen */
    const float z3[3] = { 0, 0, 0 };
    set_light(3, 0, z3, z3, (const float[]){ 0, 0, 8 }, z3);
    in[0].nrm[1][2] = 2;                            /* tangente: d1 = (0,0,1) · (0,0,2) = 2 */
    in[0].nrm[2][2] = -0.5f;                        /* binormal: d2 = -0.5 */
    xf.reg[R_TEXMTXINFO + 1] = TI_TYPE(1) | TI_EMB(0, 3);
    /* 2: del color 0 después de la luz */
    xf.reg[R_COLOR0] = CH_MATSRC_VTX;
    memcpy(in[0].col[0], (const uint8_t[]){ 51, 102, 0, 0 }, 4);
    xf.reg[R_TEXMTXINFO + 2] = TI_TYPE(2);
    xf.tex_scale[0][0] = 2;                         /* la escala va después del emboss */
    xf.tex_scale[2][1] = 3;

    gx_xform(&xf, in, 1, out);
    CHECK_EQ_U64(fbits(out[0].tex[0][0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].tex[1][0]), fbits(2.5f));
    CHECK_EQ_U64(fbits(out[0].tex[1][1]), fbits(-0.25f));
    CHECK_EQ_U64(fbits(out[0].tex[1][2]), fbits(1.0f));
    const float c51 = 51.0f, c102 = 102.0f, c255 = 255.0f;
    CHECK_EQ_U64(fbits(out[0].tex[2][0]), fbits(c51 / c255));
    CHECK_EQ_U64(fbits(out[0].tex[2][1]), fbits((c102 / c255) * 3.0f));
    CHECK_EQ_U64(fbits(out[0].tex[2][2]), fbits(1.0f));
}

static void test_xform_num_texgens(void)
{
    reset();
    xf.reg[R_NUMTEXGEN] = 5;
    CHECK_EQ_U64(gx_xf_num_texgens(&xf), 5);
    xf.reg[R_NUMTEXGEN] = 0xF;
    CHECK_EQ_U64(gx_xf_num_texgens(&xf), 8);
    xf.reg[R_NUMTEXGEN] = 0x12;                     /* solo 4 bits */
    CHECK_EQ_U64(gx_xf_num_texgens(&xf), 2);
}

static void test_xform_to_int(void)
{
    CHECK_EQ_U64((uint32_t)gx_xf_to_int(NAN), 0x80000000u);
    CHECK_EQ_U64((uint32_t)gx_xf_to_int(3e9f), 0x80000000u);
    CHECK_EQ_U64((uint32_t)gx_xf_to_int(-3e9f), 0x80000000u);
    CHECK_EQ_U64((uint32_t)gx_xf_to_int(INFINITY), 0x80000000u);
    CHECK_EQ_U64((uint32_t)gx_xf_to_int(-2147483648.0f), 0x80000000u);
    CHECK_EQ_U64(gx_xf_to_int(2147483520.0f), 2147483520u);
    CHECK_EQ_U64(gx_xf_to_int(255.9f), 255);
    CHECK_EQ_U64(gx_xf_to_int(-0.5f), 0);
    CHECK_EQ_U64((uint32_t)gx_xf_to_int(-7.9f), (uint32_t)-7);
}

/* Un bloque de 64 vértices: cada uno con su posición */
static void test_xform_block(void)
{
    static gx_vtx_in_t bin[GX_VTX_BLOCK];
    static gx_vtx_out_t bout[GX_VTX_BLOCK];
    reset();
    memset(bin, 0, sizeof bin);
    set_mat(0, (const float[]){ 1, 0, 0, 0.5f, 0, 1, 0, 0, 0, 0, 1, -1 }, 12);
    for (int v = 0; v < GX_VTX_BLOCK; v++) {
        bin[v].pos[0] = (float)v;
        bin[v].nrm[0][2] = 1;
    }
    gx_xform(&xf, bin, GX_VTX_BLOCK, bout);
    for (int v = 0; v < GX_VTX_BLOCK; v++) {
        CHECK_EQ_U64(fbits(bout[v].mv[0]), fbits((float)v + 0.5f));
        CHECK_EQ_U64(fbits(bout[v].mv[2]), fbits(-1.0f));
    }
}

const test_case_t xform_tests[] = {
    { "xform: posición y proyecciones", test_xform_position },
    { "xform: normales", test_xform_normals },
    { "xform: color sin luces", test_xform_color_no_lighting },
    { "xform: luces y difusa", test_xform_lights_diffuse },
    { "xform: luz sobre el vértice (NaN)", test_xform_light_nan },
    { "xform: atenuación spot y especular", test_xform_light_attn },
    { "xform: coordenadas regulares", test_xform_texgen_regular },
    { "xform: matriz final (dual tex)", test_xform_texgen_dual },
    { "xform: emboss y desde color", test_xform_texgen_emboss_color },
    { "xform: cantidad de coordenadas", test_xform_num_texgens },
    { "xform: float a int como x86", test_xform_to_int },
    { "xform: bloque de 64", test_xform_block },
    { NULL, NULL },
};
