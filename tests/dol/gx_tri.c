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
 * DOL de prueba "gx_tri" (Fase 2, etapa B): vértices cargados y
 * transformados. Con la mini-librería propia (gxlib y gxvtx, sin libogc):
 *
 *   1. todos los tipos de primitiva, con cantidades que sobran y que no
 *      alcanzan;
 *   2. la posición en cada formato (u8, s8, u16, s16, float; XY y XYZ; con y
 *      sin ByteDequant), indexada de 8 y de 16 bits con vértices descartados,
 *      y con otra matriz;
 *   3. el índice de matriz de posición en cada vértice;
 *   4. los colores en cada formato, directos e indexados, y solo el color 1;
 *   5. normales (N, NTB con tres índices, float) y las 8 luces con cada
 *      función de difusa y de atenuación, en los dos canales; la luz en el
 *      mismo punto que el vértice;
 *   6. las 8 coordenadas de textura: cada fila de origen, ST y STQ, AB11 y
 *      ABC1, emboss, desde los colores, q = 0, la matriz final (dual tex) con
 *      y sin normalizar, índices de matriz en el vértice y la escala de BP;
 *   7. la proyección ortográfica;
 *   8. primitivas seguidas en el mismo lote de Dolphin;
 *   9. la caché de normales;
 *  10. una display list con primitivas;
 *  11. una tira de 150 vértices (más de dos bloques de 64);
 *  12. XF y BP que no coinciden (Dolphin no dibuja).
 *
 * Lo que se compara es cada vértice transformado, bit a bit, contra el
 * renderizador por software de Dolphin (tests/dol/dolphin/gx_tri.vtx.txt); la
 * salida del programa solo dice que la GPU terminó cada parte.
 *
 * Solo para el DOL: no compila en la PC.
 */
#include "gxlib.h"
#include "rt.h"

#define FIFO_SIZE (64u * 1024u)
#define MAX_WAIT  2000000u

static uint8_t fifo[FIFO_SIZE] __attribute__((aligned(32)));
static uint8_t arr_pos[64] __attribute__((aligned(32)));
static uint8_t arr_nrm[256] __attribute__((aligned(32)));
static uint8_t arr_col0[64] __attribute__((aligned(32)));
static uint8_t arr_col1[64] __attribute__((aligned(32)));
static uint8_t arr_tex1[64] __attribute__((aligned(32)));
static uint8_t arr_tex3[64] __attribute__((aligned(32)));
static uint8_t dl_mem[512] __attribute__((aligned(32)));

static uint32_t token;
static int fails;

static const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
static const float ident33[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
static const float m0[12] = { 0.8f, 0.0f, 0.6f, 0.5f, 0.0f, 1.0f, 0.0f, -0.25f, -0.6f, 0.0f, 0.8f, -10.0f };
static const float m3[12] = { 2.0f, 0.125f, 0.0f, 1.0f, 0.0f, 1.5f, -0.3f, 2.0f, 0.1f, 0.0f, 0.9f, -20.0f };
static const float m6[12] = { 0.70710678f, -0.70710678f, 0.0f, 0.0f, 0.70710678f, 0.70710678f,
                              0.0f, 0.0f, 0.0f, 0.0f, 1.0f, -5.0f };
static const float n0[9] = { 0.8f, 0.0f, 0.6f, 0.0f, 1.0f, 0.0f, -0.6f, 0.0f, 0.8f };
static const float n3[9] = { 2.0f, 0.125f, 0.0f, 0.0f, 1.5f, -0.3f, 0.1f, 0.0f, 0.9f };
static const float n6[9] = { 0.70710678f, -0.70710678f, 0.0f, 0.70710678f, 0.70710678f, 0.0f,
                             0.0f, 0.0f, 1.0f };
static const float persp[6] = { 1.2990381f, 0.1f, 1.7320508f, -0.05f, -0.001001001f, -1.001001f };
static const float ortho[6] = { 0.003125f, -1.0f, -0.0041666668f, 1.0f, -0.001f, -1.0f };

/* La GPU termina lo que hay en el FIFO: un token (que vacía el lote de
 * Dolphin), esperar el FIFO vacío y que el token llegue */
static void sync_gpu(const char *what)
{
    token++;
    gx_bp(BP_TOKEN, token);
    gx_flush();
    if (!gx_wait_idle(MAX_WAIT)) {
        OSReport("gx_tri: %s: la GPU no vacio el FIFO\n", what);
        fails++;
        return;
    }
    uint32_t n = 0;
    while (PE_REG(PE_TOKEN) != (uint16_t)token && n < MAX_WAIT)
        n++;
    if (n == MAX_WAIT) {
        OSReport("gx_tri: %s: el token %u no llego\n", what, (unsigned)token);
        fails++;
        return;
    }
    OSReport("gx_tri: %s\n", what);
}

static void genmode(uint32_t texgens, uint32_t chans)
{
    gx_bp(BP_GENMODE, texgens | chans << 4);
    gx_xf1(XF_NUMTEXGEN, texgens);
    gx_xf1(XF_NUMCHAN, chans);
}

static void pos3(float x, float y, float z)
{
    gx_f32(x);
    gx_f32(y);
    gx_f32(z);
}

/* ---- 0. estado inicial -------------------------------------------------------------- */

static void setup(void)
{
    gx_init_fifo(fifo, FIFO_SIZE, FIFO_SIZE - 16384, FIFO_SIZE / 2, CP_CR_READ | CP_CR_LINK);
    genmode(0, 1);
    gx_xf1(XF_DUALTEX, 0);
    gx_projection(persp, 0);
    static const float viewport[6] = { 320.0f, -240.0f, 16777215.0f, 342.0f, 262.0f, 16777215.0f };
    gx_xf_floats(XF_VIEWPORT, 6, viewport);
    gx_pos_matrix(0, m0);
    gx_pos_matrix(3, m3);
    gx_pos_matrix(6, m6);
    gx_pos_matrix(9, ident);
    gx_normal_matrix(0, n0);
    gx_normal_matrix(3, n3);
    gx_normal_matrix(6, n6);
    gx_normal_matrix(9, ident33);
    gx_matindex(0, 0);
    gx_xf1(XF_COLOR0, CH_MATSRC_VTX);
    gx_xf1(XF_ALPHA0, CH_MATSRC_VTX);
    gx_xf1(XF_COLOR0 + 1, CH_MATSRC_VTX);
    gx_xf1(XF_ALPHA0 + 1, CH_MATSRC_VTX);
    gx_xf1(XF_MAT0, 0x80808080u);
    gx_xf1(XF_MAT0 + 1, 0x40404040u);
    gx_xf1(XF_AMB0, 0x20202020u);
    gx_xf1(XF_AMB0 + 1, 0x10101010u);
    sync_gpu("estado inicial");
}

/* ---- 1. tipos de primitiva ---------------------------------------------------------- */

static void vtx_pf(uint32_t k)
{
    pos3((float)(int)(k % 7u) * 0.75f - 2.25f, (float)(int)(k % 5u) * 1.25f - 2.5f,
         -1.5f - (float)(int)k * 0.375f);
    gx_u32(0x10203040u + k * 0x0B0D0F11u);
}

static void fmt_pf(void)
{
    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_8888), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 0);
}

static void primitives(void)
{
    static const uint8_t prims[][2] = {
        { GX_TRIANGLES, 9 }, { GX_TRIANGLE_STRIP, 5 }, { GX_TRIANGLE_FAN, 6 }, { GX_QUADS, 7 },
        { GX_QUADS_2, 4 },   { GX_LINES, 5 },          { GX_LINE_STRIP, 3 },   { GX_POINTS, 2 },
        { GX_TRIANGLES, 4 }, { GX_TRIANGLE_STRIP, 2 }, { GX_QUADS, 6 },        { GX_LINE_STRIP, 1 },
        { GX_TRIANGLE_FAN, 3 }, { GX_LINES, 1 },       { GX_QUADS, 3 },
    };
    fmt_pf();
    uint32_t k = 0;
    for (uint32_t i = 0; i < sizeof prims / sizeof prims[0]; i++) {
        gx_begin(prims[i][0], 0, prims[i][1]);
        for (uint32_t j = 0; j < prims[i][1]; j++)
            vtx_pf(k++);
    }
    sync_gpu("1. tipos de primitiva");
}

/* ---- 2. formatos de posición -------------------------------------------------------- */

static void positions(void)
{
    gx_vat(1, VAT_POS(1, FMT_S16, 8) | VAT_DEQUANT | VAT_COL0(COL_8888), 0, 0);
    gx_vat(2, VAT_POS(0, FMT_U8, 3) | VAT_COL0(COL_8888), 0, 0);
    gx_vat(3, VAT_POS(1, FMT_S8, 6) | VAT_DEQUANT | VAT_COL0(COL_8888), 0, 0);
    gx_vat(4, VAT_POS(1, FMT_U16, 12) | VAT_DEQUANT | VAT_COL0(COL_8888), 0, 0);
    gx_vat(5, VAT_POS(0, FMT_F32, 0) | VAT_COL0(COL_8888), 0, 0);
    gx_vat(6, VAT_POS(1, FMT_U8, 2) | VAT_DEQUANT | VAT_COL0(COL_8888), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 0);

    static const int16_t s16[3][3] = { { 0x0180, -0x0180, -0x0A00 }, { -0x0333, 0x0222, -0x0C80 },
                                       { 0x7FFF, -0x8000, -0x0101 } };
    gx_begin(GX_TRIANGLES, 1, 3);
    for (int j = 0; j < 3; j++) {
        for (int c = 0; c < 3; c++)
            gx_u16((uint16_t)s16[j][c]);
        gx_u32(0xFF000000u >> (8 * j) | 0xFFu);
    }
    static const uint8_t u8xy[3][2] = { { 0, 255 }, { 17, 3 }, { 200, 100 } };
    gx_begin(GX_TRIANGLES, 2, 3);
    for (int j = 0; j < 3; j++) {
        gx_u8(u8xy[j][0]);
        gx_u8(u8xy[j][1]);
        gx_u32(0x11223344u * (uint32_t)(j + 1));
    }
    static const int8_t s8[3][3] = { { 64, -64, -127 }, { -128, 127, -1 }, { 3, 5, -90 } };
    gx_begin(GX_TRIANGLES, 3, 3);
    for (int j = 0; j < 3; j++) {
        for (int c = 0; c < 3; c++)
            gx_u8((uint8_t)s8[j][c]);
        gx_u32(0x80402010u >> j);
    }
    static const uint16_t u16[3][3] = { { 0x1000, 0x2000, 0xF000 }, { 0xFFFF, 0x0001, 0x8000 },
                                        { 0x1234, 0x5678, 0x9ABC } };
    gx_begin(GX_TRIANGLES, 4, 3);
    for (int j = 0; j < 3; j++) {
        for (int c = 0; c < 3; c++)
            gx_u16(u16[j][c]);
        gx_u32(0xA0B0C0D0u ^ (uint32_t)j);
    }
    gx_begin(GX_TRIANGLES, 5, 3);
    for (int j = 0; j < 3; j++) {
        gx_f32(0.1f * (float)(j + 1));
        gx_f32(-0.3f * (float)j);
        gx_u32(0x01020304u << j);
    }
    static const uint8_t u8xyz[3][3] = { { 4, 8, 12 }, { 255, 0, 128 }, { 1, 2, 3 } };
    gx_begin(GX_TRIANGLES, 6, 3);
    for (int j = 0; j < 3; j++) {
        for (int c = 0; c < 3; c++)
            gx_u8(u8xyz[j][c]);
        gx_u32(0xCAFEBABEu + (uint32_t)j);
    }

    /* Con las matrices 3 y 6 por XF MatrixIndexA */
    for (uint32_t m = 3; m <= 6; m += 3) {
        gx_matindex(m, 0);
        gx_begin(GX_TRIANGLES, 5, 3);
        for (int j = 0; j < 3; j++) {
            gx_f32(1.5f - (float)j);
            gx_f32(0.25f * (float)j);
            gx_u32(0x55AA55AAu);
        }
    }
    gx_matindex(0, 0);

    /* Indexadas: s16 XYZ en el arreglo 0, de a 8 bytes */
    static const int16_t table[8][3] = {
        { 0, 0, -2560 },     { 512, 0, -2600 },  { 0, 512, -2700 },   { -300, -300, -3000 },
        { 1000, 50, -5000 }, { 77, -77, -1500 }, { -1024, 256, -900 }, { 3, 4, -5 },
    };
    for (int e = 0; e < 8; e++)
        for (int c = 0; c < 3; c++) {
            arr_pos[8 * e + 2 * c] = (uint8_t)((uint16_t)table[e][c] >> 8);
            arr_pos[8 * e + 2 * c + 1] = (uint8_t)table[e][c];
        }
    dc_flush(arr_pos, sizeof arr_pos);
    gx_array(0, arr_pos, 8);
    gx_vtxdesc(VCD_POS(VTX_INDEX8) | VCD_COL0(VTX_DIRECT), 0, 0);
    static const uint8_t ix8[9] = { 0, 1, 2, 3, 0xFF, 4, 5, 6, 7 };
    gx_begin(GX_TRIANGLES, 1, 9);
    for (int j = 0; j < 9; j++) {
        gx_u8(ix8[j]);
        gx_u32(0x00FF00FFu + (uint32_t)j * 0x01000100u);
    }
    gx_vtxdesc(VCD_POS(VTX_INDEX16) | VCD_COL0(VTX_DIRECT), 0, 0);
    static const uint16_t ix16[5] = { 2, 0xFFFF, 1, 3, 0 };
    gx_begin(GX_TRIANGLE_STRIP, 1, 5);
    for (int j = 0; j < 5; j++) {
        gx_u16(ix16[j]);
        gx_u32(0xFFFFFFFFu - (uint32_t)j);
    }
    sync_gpu("2. formatos de posicion");
}

/* ---- 3. matriz de posición en el vértice -------------------------------------------- */

static void pos_matrix_index(void)
{
    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_8888), 0, 0);
    gx_vtxdesc(VCD_POSMTX | VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 0);
    static const uint8_t idx[7] = { 0, 3, 6, 0xC3, 0x46, 9, 0x00 };
    gx_begin(GX_TRIANGLE_FAN, 0, 7);
    for (int j = 0; j < 7; j++) {
        gx_u8(idx[j]);
        pos3(0.5f * (float)j, 1.0f - 0.25f * (float)j, -0.5f * (float)j);
        gx_u32(0x30405060u + (uint32_t)j);
    }
    sync_gpu("3. matriz de posicion en el vertice");
}

/* ---- 4. colores --------------------------------------------------------------------- */

static void colors(void)
{
    genmode(0, 2);
    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_565) | VAT_COL1(COL_888), 0, 0);
    gx_vat(1, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_888X) | VAT_COL1(COL_4444), 0, 0);
    gx_vat(2, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_6666) | VAT_COL1(COL_8888), 0, 0);
    gx_vat(3, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_8888) | VAT_COL1(COL_565), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT) | VCD_COL1(VTX_DIRECT), 0, 0);

    gx_begin(GX_TRIANGLES, 0, 3);
    for (uint32_t j = 0; j < 3; j++) {
        pos3(-1.0f + (float)j, 0.5f, -3.0f);
        gx_u16(0x8410u + j * 0x1111u);                      /* 565 */
        gx_u8(0x12 * j);                                     /* 888 */
        gx_u8(0xFF - j);
        gx_u8(0x80 + j);
    }
    gx_begin(GX_TRIANGLES, 1, 3);
    for (uint32_t j = 0; j < 3; j++) {
        pos3(-1.0f + (float)j, -0.5f, -3.5f);
        gx_u32(0x9ABCDE00u + j);                             /* 888x */
        gx_u16(0x1234u + j * 0x4321u);                       /* 4444 */
    }
    gx_begin(GX_TRIANGLES, 2, 3);
    for (uint32_t j = 0; j < 3; j++) {
        pos3(-1.0f + (float)j, 1.5f, -4.0f);
        const uint32_t c = 0xFC056Au ^ (j * 0x123456u);      /* 6666 */
        gx_u8(c >> 16);
        gx_u8(c >> 8);
        gx_u8(c);
        gx_u32(0x01234567u * (j + 1));                       /* 8888 */
    }

    /* Indexados: el 0 de a 4 bytes (8888), el 1 de a 2 (565) */
    for (uint32_t e = 0; e < 16; e++) {
        arr_col0[4 * e] = (uint8_t)(e * 16);
        arr_col0[4 * e + 1] = (uint8_t)(255 - e);
        arr_col0[4 * e + 2] = (uint8_t)(e * 7);
        arr_col0[4 * e + 3] = (uint8_t)(128 + e);
        arr_col1[2 * e] = (uint8_t)(0xF8 - e);
        arr_col1[2 * e + 1] = (uint8_t)(0x1F + e * 3);
    }
    dc_flush(arr_col0, sizeof arr_col0);
    dc_flush(arr_col1, sizeof arr_col1);
    gx_array(2, arr_col0, 4);
    gx_array(3, arr_col1, 2);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_INDEX8) | VCD_COL1(VTX_INDEX16), 0, 0);
    gx_begin(GX_TRIANGLE_STRIP, 3, 4);
    for (uint32_t j = 0; j < 4; j++) {
        pos3((float)j * 0.5f, (float)(j & 1u), -2.5f);
        gx_u8(j * 3 + 1);
        gx_u16(15 - j * 2);
    }

    /* Solo el color 1: va al canal 0 */
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL1(VTX_DIRECT), 0, 0);
    gx_begin(GX_TRIANGLES, 2, 3);
    for (uint32_t j = 0; j < 3; j++) {
        pos3(0.25f * (float)j, -0.75f, -2.0f);
        gx_u32(0xDEADBEEFu - j);
    }
    /* Ninguno */
    gx_vtxdesc(VCD_POS(VTX_DIRECT), 0, 0);
    gx_begin(GX_TRIANGLES, 2, 3);
    for (uint32_t j = 0; j < 3; j++)
        pos3(0.25f * (float)j, 0.75f, -2.0f);
    sync_gpu("4. colores");
}

/* ---- 5. normales y luces ------------------------------------------------------------ */

static void lights(void)
{
    static const struct {
        uint32_t color;
        float cosatt[3], distatt[3], pos[3], dir[3];
    } l[8] = {
        { 0xFF8040C0u, { 1, 0, 0 }, { 1, 0.0625f, 0.00390625f }, { 10, 20, 5 }, { 0, -0.70710677f, -0.70710677f } },
        { 0x40FF8020u, { -2.5f, 3.5f, 0 }, { 0.5f, 0.25f, 0 }, { -30, 5, -2 }, { 0.6f, 0, 0.8f } },
        { 0x2040FFFFu, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 0, 100 }, { 0, 0, -1 } },
        { 0xFFFFFF80u, { 0.25f, 0.5f, 0.25f }, { 0, 0.1f, 0 }, { 3, -4, -12 }, { 0.57735026f, 0.57735026f, 0.57735026f } },
        { 0x80808080u, { 1, -1, 1 }, { 2, 0, 0.5f }, { -1, 1, -8 }, { 0, 1, 0 } },
        { 0x00FF00FFu, { 0, 2, -1 }, { 1, 1, 1 }, { 100, -100, 50 }, { -0.6f, 0.8f, 0 } },
        { 0xFF0000FFu, { 1.5f, 0, 0 }, { 0, 0, 0 }, { 0.5f, 0.5f, -9.5f }, { 0, 0, 1 } },
        { 0x0000FF40u, { 0.1f, 0.2f, 0.3f }, { 0.3f, 0.2f, 0.1f }, { -5, -5, -5 }, { 0.70710677f, 0, 0.70710677f } },
    };
    for (uint32_t i = 0; i < 8; i++)
        gx_light(i, l[i].color, l[i].cosatt, l[i].distatt, l[i].pos, l[i].dir);
    gx_xf1(XF_MAT0, 0xC0A08060u);
    gx_xf1(XF_MAT0 + 1, 0x7F3FBFFFu);
    gx_xf1(XF_AMB0, 0x10203040u);
    gx_xf1(XF_AMB0 + 1, 0x05060708u);
}

static void normals_and_lights(void)
{
    genmode(0, 2);
    lights();
    gx_vat(5, VAT_POS(1, FMT_F32, 0) | VAT_NRM(0, FMT_S8) | VAT_COL0(COL_8888), 0, 0);
    gx_vat(6, VAT_POS(1, FMT_S16, 4) | VAT_DEQUANT | VAT_NRM(1, FMT_S16) | VAT_INDEX3 | VAT_COL0(COL_8888), 0, 0);
    gx_vat(7, VAT_POS(1, FMT_F32, 0) | VAT_NRM(1, FMT_F32) | VAT_COL0(COL_8888), 0, 0);

    /* NTB s16 en el arreglo 1, de a 20 bytes */
    for (uint32_t e = 0; e < 12; e++)
        for (uint32_t c = 0; c < 9; c++) {
            const uint16_t v = (uint16_t)(int16_t)((int)(e * 1500u + c * 2311u) % 32768 - 16384);
            arr_nrm[20 * e + 2 * c] = (uint8_t)(v >> 8);
            arr_nrm[20 * e + 2 * c + 1] = (uint8_t)v;
        }
    dc_flush(arr_nrm, sizeof arr_nrm);
    gx_array(1, arr_nrm, 20);

    static const uint32_t cfg[2][4] = {
        { CH_MATSRC_VTX | CH_LIGHT | CH_MASK(0xFF) | CH_DIFF(2) | CH_ATTN(3),
          CH_LIGHT | CH_MASK(0x0F) | CH_DIFF(1) | CH_ATTN(1),
          CH_LIGHT | CH_AMBSRC_VTX | CH_MASK(0xF0) | CH_DIFF(0) | CH_ATTN(2),
          CH_MATSRC_VTX | CH_LIGHT | CH_MASK(0x81) | CH_DIFF(2) | CH_ATTN(0) },
        { CH_LIGHT | CH_MASK(0x3C) | CH_DIFF(1) | CH_ATTN(1),
          CH_MATSRC_VTX | CH_LIGHT | CH_AMBSRC_VTX | CH_MASK(0xC3) | CH_DIFF(0) | CH_ATTN(3),
          CH_MATSRC_VTX | CH_LIGHT | CH_MASK(0x5A) | CH_DIFF(2) | CH_ATTN(0),
          CH_LIGHT | CH_MASK(0xA5) | CH_DIFF(2) | CH_ATTN(2) },
    };
    static const int8_t n8[6][3] = { { 64, 0, 0 }, { 0, -64, 0 }, { 45, 45, 0 },
                                     { -20, 30, 64 }, { 0, 0, 0 }, { 10, -127, 33 } };
    for (int g = 0; g < 2; g++) {
        gx_xf1(XF_COLOR0, cfg[g][0]);
        gx_xf1(XF_ALPHA0, cfg[g][1]);
        gx_xf1(XF_COLOR0 + 1, cfg[g][2]);
        gx_xf1(XF_ALPHA0 + 1, cfg[g][3]);

        /* N s8 */
        gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_NRM(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 0);
        gx_begin(GX_TRIANGLES, 5, 6);
        for (int j = 0; j < 6; j++) {
            pos3(-2.0f + 0.8f * (float)j, 1.0f - 0.4f * (float)j, -1.0f - 0.7f * (float)j);
            for (int c = 0; c < 3; c++)
                gx_u8((uint8_t)n8[j][c]);
            gx_u32(0x8899AABBu ^ (uint32_t)(j * 0x01010101));
        }
        /* NTB s16 con tres índices de 16 bits */
        gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_NRM(VTX_INDEX16) | VCD_COL0(VTX_DIRECT), 0, 1);
        gx_begin(GX_TRIANGLE_STRIP, 6, 4);
        for (uint32_t j = 0; j < 4; j++) {
            gx_u16((uint16_t)(int16_t)(16 * (int)j - 24));
            gx_u16((uint16_t)(int16_t)(8 - 12 * (int)j));
            gx_u16((uint16_t)(int16_t)(-40 - 9 * (int)j));
            gx_u16(j);
            gx_u16(j + 4);
            gx_u16(11 - j);
            gx_u32(0x10E0F0A0u + j * 0x22u);
        }
        /* NTB float */
        gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_NRM(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 1);
        gx_begin(GX_TRIANGLES, 7, 3);
        for (int j = 0; j < 3; j++) {
            pos3(0.3f * (float)j, -0.6f, -6.0f + (float)j);
            pos3(0.3f, 0.4f - 0.2f * (float)j, -0.5f);
            pos3(2.0f, 0.0f, 0.25f * (float)j);
            pos3(0.0f, 0.5f, -1.0f);
            gx_u32(0x7F7F7F7Fu + (uint32_t)j);
        }
        /* La luz 6 en el mismo punto que un vértice (matriz identidad) */
        gx_matindex(9, 0);
        gx_begin(GX_TRIANGLES, 7, 3);
        for (int j = 0; j < 3; j++) {
            pos3(0.5f, 0.5f, -9.5f - (float)j);
            pos3(0.0f, 0.0f, 1.0f);
            pos3(1.0f, 0.0f, 0.0f);
            pos3(0.0f, 1.0f, 0.0f);
            gx_u32(0xFFFFFFFFu);
        }
        gx_matindex(0, 0);
    }
    sync_gpu("5. normales y luces");
}

/* ---- 6. coordenadas de textura ------------------------------------------------------- */

static void texgen_vertex(uint32_t j)
{
    gx_u8(39 + 3 * (j & 1u));                                /* texmtx 3 */
    gx_u8(48 + 3 * (j % 3u));                                /* texmtx 6 */
    pos3(-1.0f + 0.5f * (float)j, 0.75f - 0.3f * (float)j, -3.0f - 0.2f * (float)j);
    pos3(0.0f, 0.6f, 0.8f - 0.1f * (float)j);                /* N */
    pos3(1.5f, 0.25f * (float)j, 0.0f);                      /* T */
    pos3(0.0f, -0.5f, 1.0f + (float)j);                      /* B */
    gx_u32(0x33669900u + j * 0x05030201u);                   /* color 0 */
    gx_u16(0x07E0u + j * 0x0841u);                           /* color 1 (565) */
    if (j == 4)
        gx_u32(0x7FC00000u);                                 /* tex0: NaN */
    else
        gx_f32(0.125f * (float)j);
    gx_f32(1.0f - 0.0625f * (float)j);
    gx_u16(j);                                               /* tex1: índice */
    gx_u8(16 * j + 5);                                       /* tex2 u8 S */
    gx_u8(j + 2);                                            /* tex3: índice */
    gx_u16(0x0400u * j + 0x0123u);                           /* tex4 u16 ST */
    gx_u16(0xFFFFu - 0x0800u * j);
    gx_f32(-2.5f + (float)j);                                /* tex5 float S */
    gx_u16((uint16_t)(int16_t)(-7 * (int)j));                /* tex6 s16 ST */
    gx_u16((uint16_t)(int16_t)(300 + (int)j));
    gx_u8(j * 31);                                           /* tex7 u8 ST */
    gx_u8(255 - j * 9);
}

static void texcoords(void)
{
    genmode(8, 2);
    gx_xf1(XF_COLOR0, CH_MATSRC_VTX);
    gx_xf1(XF_ALPHA0, CH_MATSRC_VTX);
    gx_xf1(XF_COLOR0 + 1, CH_MATSRC_VTX | CH_LIGHT | CH_MASK(0x22) | CH_DIFF(2));
    gx_xf1(XF_ALPHA0 + 1, CH_MATSRC_VTX);

    for (int e = 0; e < 16; e++) {
        const uint16_t s = (uint16_t)(int16_t)(e * 97 - 700), t = (uint16_t)(int16_t)(e * -45 + 333);
        arr_tex1[4 * e] = (uint8_t)(s >> 8);
        arr_tex1[4 * e + 1] = (uint8_t)s;
        arr_tex1[4 * e + 2] = (uint8_t)(t >> 8);
        arr_tex1[4 * e + 3] = (uint8_t)t;
        arr_tex3[2 * e] = (uint8_t)(int8_t)(e * 9 - 64);
        arr_tex3[2 * e + 1] = (uint8_t)(int8_t)(100 - e * 13);
    }
    dc_flush(arr_tex1, sizeof arr_tex1);
    dc_flush(arr_tex3, sizeof arr_tex3);
    gx_array(5, arr_tex1, 4);
    gx_array(7, arr_tex3, 2);

    /* Matrices de textura en 30, 33, ..., 51; la 33 con la fila de q en cero */
    static const float tm[8][12] = {
        { 0.5f, 0, 0, 0.5f, 0, -0.5f, 0, 0.5f, 0, 0, 1, 0 },
        { 1.5f, 0, 0, 0.25f, 0, -2, 0, 0.5f, 0, 0, 0, 0 },
        { 0.25f, 0.5f, 0.75f, 0, 1, -1, 0.5f, 0.125f, 0.3f, 0.3f, 0.3f, 1 },
        { 2, 0, 0, -1, 0, 2, 0, -1, 0, 0, 1, 0 },
        { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f },
        { -1, 0, 0, 1, 0, -1, 0, 1, 0, 0, -1, 1 },
        { 0.7f, -0.7f, 0, 0, 0.7f, 0.7f, 0, 0, 0, 0, 0.5f, 2 },
        { 3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0 },
    };
    for (uint32_t i = 0; i < 8; i++)
        gx_pos_matrix(30 + 3 * i, tm[i]);
    static const float post[6][12] = {
        { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 },
        { 0.5f, 0, 0, 0.5f, 0, 0.5f, 0, 0.5f, 0, 0, 1, 0 },
        { 2, 1, 0, 0, 0, 2, 1, 0, 0, 0, 0, 1 },
        { 0.3f, 0.6f, 0.9f, 0.1f, -0.2f, 0.4f, 0.8f, 0, 0, 0, 0, 0 },
        { 1, 0, 0, 0.125f, 0, 1, 0, -0.125f, 0, 0, 2, 0 },
        { 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0.5f },
    };
    for (uint32_t i = 0; i < 6; i++)
        gx_post_matrix(5 * i, post[i]);
    gx_matindex(0 | 30u << 6 | 33u << 12 | 36u << 18 | 39u << 24, 42u | 45u << 6 | 48u << 12 | 51u << 18);

    /* Escalas de BP en las coordenadas 0, 3 y 7 */
    gx_bp(BP_SU_SSIZE, 255);
    gx_bp(BP_SU_TSIZE, 127);
    gx_bp(BP_SU_SSIZE + 6, 63);
    gx_bp(BP_SU_TSIZE + 6, 31);
    gx_bp(BP_SU_SSIZE + 14, 1023);
    gx_bp(BP_SU_TSIZE + 14, 0x10000u | 511);                 /* con el bit de "range bias" */

    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_NRM(1, FMT_F32) | VAT_COL0(COL_8888) | VAT_COL1(COL_565) |
              VAT_TEX0(1, FMT_F32, 0) | VAT_DEQUANT,
           1u | (uint32_t)FMT_S16 << 1 | 8u << 4 |                     /* tex1 s16 ST, 8 */
           (uint32_t)FMT_U8 << 10 | 4u << 13 |                         /* tex2 u8 S, 4 */
           1u << 18 | (uint32_t)FMT_S8 << 19 | 6u << 22 |              /* tex3 s8 ST, 6 */
           1u << 27 | (uint32_t)FMT_U16 << 28,                         /* tex4 u16 ST */
           10u |                                                       /* tex4: 10 */
           (uint32_t)FMT_F32 << 6 |                                    /* tex5 float S */
           1u << 14 | (uint32_t)FMT_S16 << 15 |                        /* tex6 s16 ST, 0 */
           1u << 23 | (uint32_t)FMT_U8 << 24 | 7u << 27);              /* tex7 u8 ST, 7 */
    gx_vtxdesc(VCD_TEXMTX(3) | VCD_TEXMTX(6) | VCD_POS(VTX_DIRECT) | VCD_NRM(VTX_DIRECT) |
                   VCD_COL0(VTX_DIRECT) | VCD_COL1(VTX_DIRECT),
               VCD_TEX(0, VTX_DIRECT) | VCD_TEX(1, VTX_INDEX16) | VCD_TEX(2, VTX_DIRECT) |
                   VCD_TEX(3, VTX_INDEX8) | VCD_TEX(4, VTX_DIRECT) | VCD_TEX(5, VTX_DIRECT) |
                   VCD_TEX(6, VTX_DIRECT) | VCD_TEX(7, VTX_DIRECT),
               1);

    static const uint32_t pass[3][8] = {
        { TG_ROW(5), TG_STQ | TG_ABC1 | TG_ROW(0), TG_STQ | TG_ABC1 | TG_ROW(1), TG_ROW(8),
          TG_ABC1 | TG_ROW(3), TG_TYPE(1) | TG_EMBOSS(0, 2), TG_STQ | TG_ABC1 | TG_ROW(4), TG_TYPE(2) },
        { TG_STQ | TG_ROW(6), TG_ABC1 | TG_ROW(7), TG_STQ | TG_ABC1 | TG_ROW(9), TG_ROW(10),
          TG_STQ | TG_ROW(11), TG_TYPE(3), TG_STQ | TG_ABC1 | TG_ROW(12), TG_TYPE(1) | TG_EMBOSS(4, 5) },
        { TG_ROW(5), TG_STQ | TG_ROW(0), TG_TYPE(1) | TG_EMBOSS(1, 7), 0, 0, 0, 0, 0 },
    };
    static const uint32_t postinfo[8] = { 0, 5 | 1u << 8, 10, 15 | 1u << 8, 20, 0, 25 | 1u << 8, 0 };
    for (int p = 0; p < 3; p++) {
        if (p == 2)
            genmode(3, 2);                                   /* solo 3: las otras no se escalan */
        gx_xf1(XF_DUALTEX, p == 1);
        gx_xf(XF_TEXMTXINFO, 8, pass[p]);
        gx_xf(XF_POSTMTXINFO, 8, postinfo);
        gx_begin(GX_TRIANGLES, 0, 6);
        for (uint32_t j = 0; j < 6; j++)
            texgen_vertex(j);
        sync_gpu(p == 0 ? "6. coordenadas: filas y emboss" :
                 p == 1 ? "6. coordenadas: matriz final" : "6. coordenadas: tres");
    }
    gx_xf1(XF_DUALTEX, 0);
    gx_bp(BP_SU_SSIZE, 0);
    gx_bp(BP_SU_TSIZE, 0);
    gx_matindex(0, 0);
}

/* ---- 7. ortográfica ---------------------------------------------------------------- */

static void orthographic(void)
{
    genmode(0, 1);
    gx_projection(ortho, 1);
    fmt_pf();
    gx_begin(GX_TRIANGLE_STRIP, 0, 6);
    for (uint32_t j = 0; j < 6; j++) {
        pos3(100.0f * (float)j - 250.0f, 37.5f * (float)(j & 1u), -0.5f - 7.0f * (float)j);
        gx_u32(0x0F1E2D3Cu * (j + 1));
    }
    gx_projection(persp, 0);
    sync_gpu("7. proyeccion ortografica");
}

/* ---- 8. un lote de Dolphin con varias primitivas ----------------------------------- */

static void batches(void)
{
    fmt_pf();
    gx_vat(1, VAT_POS(1, FMT_S16, 8) | VAT_DEQUANT | VAT_COL0(COL_8888), 0, 0);
    for (uint32_t k = 0; k < 3; k++) {
        gx_begin(GX_TRIANGLES, 0, 3);
        for (uint32_t j = 0; j < 3; j++)
            vtx_pf(10 * k + j);
        /* s16 y float dan el mismo formato nativo: no se vacía el lote */
        gx_begin(GX_TRIANGLES, 1, 3);
        for (uint32_t j = 0; j < 3; j++) {
            gx_u16(0x0100u * j);
            gx_u16(0x0080u * k);
            gx_u16((uint16_t)(int16_t)(-0x0600 - 0x40 * (int)j));
            gx_u32(0x99887766u + j);
        }
    }
    /* Una matriz nueva por XF (vacía el lote) */
    static const float mb[12] = { 1, 0.5f, 0, 0, 0, 1, 0.5f, 0, 0.5f, 0, 1, -7 };
    gx_pos_matrix(0, mb);
    gx_begin(GX_TRIANGLES, 0, 3);
    for (uint32_t j = 0; j < 3; j++)
        vtx_pf(40 + j);
    /* Otro índice de matriz (la escritura de CP vacía el lote) */
    gx_matindex(3, 0);
    gx_begin(GX_TRIANGLES, 0, 3);
    for (uint32_t j = 0; j < 3; j++)
        vtx_pf(50 + j);
    gx_matindex(0, 0);
    gx_pos_matrix(0, m0);
    gx_begin(GX_POINTS, 0, 2);
    vtx_pf(60);
    vtx_pf(61);
    sync_gpu("8. un lote con varias primitivas");
}

/* ---- 9. caché de normales ---------------------------------------------------------- */

static void tri_no_normal(float z)
{
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 0);
    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_8888), 0, 0);
    gx_begin(GX_TRIANGLES, 0, 3);
    for (int j = 0; j < 3; j++) {
        pos3((float)j, 1.0f, z);
        gx_u32(0x40404040u);
    }
}

static void normal_cache(void)
{
    genmode(0, 1);
    gx_xf1(XF_COLOR0, CH_LIGHT | CH_MASK(0x01) | CH_DIFF(2));
    gx_xf1(XF_ALPHA0, CH_MATSRC_VTX);
    gx_vat(7, VAT_POS(1, FMT_F32, 0) | VAT_NRM(1, FMT_F32) | VAT_COL0(COL_8888), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_NRM(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 1);
    gx_begin(GX_TRIANGLES, 7, 3);
    for (int j = 0; j < 3; j++) {
        pos3(0.0f, (float)j, -4.0f);
        pos3(0.1f * (float)j, 0.9f, 0.3f);
        pos3(1.0f, 0.5f * (float)j, 0.0f);
        pos3(0.0f, 0.25f, 0.75f * (float)j);
        gx_u32(0x12345678u);
    }
    sync_gpu("9. cache: NTB");
    tri_no_normal(-5.0f);
    sync_gpu("9. cache: sin normal despues de NTB");

    gx_vat(5, VAT_POS(1, FMT_F32, 0) | VAT_NRM(0, FMT_S8) | VAT_COL0(COL_8888), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_NRM(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 0);
    gx_begin(GX_TRIANGLES, 5, 3);
    for (int j = 0; j < 3; j++) {
        pos3(1.0f, (float)j, -4.5f);
        gx_u8((uint8_t)(int8_t)(10 * j - 30));
        gx_u8(50);
        gx_u8((uint8_t)(int8_t)(-20 - j));
        gx_u32(0x87654321u);
    }
    sync_gpu("9. cache: N");
    tri_no_normal(-5.5f);
    sync_gpu("9. cache: sin normal despues de N");

    /* El último vértice se descarta: la caché no cambia */
    gx_array(0, arr_pos, 8);
    gx_vat(4, VAT_POS(1, FMT_S16, 8) | VAT_DEQUANT | VAT_NRM(0, FMT_S8) | VAT_COL0(COL_8888), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_INDEX8) | VCD_NRM(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 0);
    gx_begin(GX_TRIANGLES, 4, 4);
    for (uint32_t j = 0; j < 4; j++) {
        gx_u8(j == 3 ? 0xFF : j);
        gx_u8(0);
        gx_u8((uint8_t)(int8_t)(60 - 40 * (int)j));
        gx_u8(5 * j);
        gx_u32(0x0A0B0C0Du);
    }
    sync_gpu("9. cache: el ultimo descartado");
    tri_no_normal(-6.0f);
    gx_xf1(XF_COLOR0, CH_MATSRC_VTX);
    sync_gpu("9. cache: sin normal otra vez");
}

/* ---- 10. display list --------------------------------------------------------------- */

static void display_list(void)
{
    gx_dl_t dl = { dl_mem, 0, sizeof dl_mem };
    dl_cp(&dl, CPR_VAT_A + 2, VAT_POS(1, FMT_S16, 6) | VAT_DEQUANT | VAT_COL0(COL_4444));
    dl_cp(&dl, CPR_VCD_LO, VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT));
    dl_cp(&dl, CPR_VCD_HI, 0);
    dl_xf1(&dl, XF_INVTXSPEC, 1);
    dl_u8(&dl, GX_TRIANGLE_STRIP | 2);
    dl_u16(&dl, 5);
    for (uint32_t j = 0; j < 5; j++) {
        dl_u16(&dl, (uint16_t)(int16_t)(64 * (int)j - 100));
        dl_u16(&dl, (uint16_t)(int16_t)(32 * (int)(j & 1u)));
        dl_u16(&dl, (uint16_t)(int16_t)(-300 - 10 * (int)j));
        dl_u16(&dl, 0x1234u + j * 0x1111u);
    }
    const uint32_t len = dl_end(&dl);
    gx_call_dl(dl_mem, len);
    sync_gpu("10. display list");
}

/* ---- 11. una tira larga -------------------------------------------------------------- */

static void long_strip(void)
{
    gx_vat(1, VAT_POS(1, FMT_S16, 8) | VAT_DEQUANT | VAT_COL0(COL_8888), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), 0, 0);
    gx_begin(GX_TRIANGLE_STRIP, 1, 150);
    for (uint32_t j = 0; j < 150; j++) {
        gx_u16((uint16_t)(int16_t)((int)(j % 13u) * 40 - 240));
        gx_u16((uint16_t)(int16_t)((int)(j / 13u) * 30 - 150));
        gx_u16((uint16_t)(int16_t)(-2560 - 16 * (int)j));
        gx_u32(j * 0x01020304u);
    }
    sync_gpu("11. tira de 150");
}

/* ---- 12. XF y BP distintos ----------------------------------------------------------- */

static void mismatch(void)
{
    genmode(0, 1);
    fmt_pf();
    gx_xf1(XF_NUMTEXGEN, 1);                                 /* BP sigue con 0 */
    gx_begin(GX_TRIANGLES, 0, 3);
    for (uint32_t j = 0; j < 3; j++)
        vtx_pf(70 + j);
    gx_xf1(XF_NUMTEXGEN, 0);
    gx_begin(GX_TRIANGLES, 0, 3);
    for (uint32_t j = 0; j < 3; j++)
        vtx_pf(73 + j);
    gx_xf1(XF_NUMCHAN, 2);                                   /* BP sigue con 1 */
    gx_begin(GX_TRIANGLES, 0, 3);
    for (uint32_t j = 0; j < 3; j++)
        vtx_pf(76 + j);
    gx_xf1(XF_NUMCHAN, 1);
    gx_begin(GX_TRIANGLES, 0, 3);
    for (uint32_t j = 0; j < 3; j++)
        vtx_pf(79 + j);
    sync_gpu("12. XF y BP distintos");
}

int main(void)
{
    OSReport("gx_tri: vertices cargados y transformados\n");
    setup();
    primitives();
    positions();
    pos_matrix_index();
    colors();
    normals_and_lights();
    texcoords();
    orthographic();
    batches();
    normal_cache();
    display_list();
    long_strip();
    mismatch();
    OSReport("gx_tri: %u tokens; %s\n", (unsigned)token, fails ? "FALLO" : "OK");
    exit(fails ? 1 : 0);
}
