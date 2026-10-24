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
 * Fase 2, etapa B: el cargador de vértices (src/gx/vtx.c). Los valores
 * esperados salen de VertexLoaderX64.cpp y SWVertexLoader.cpp de Dolphin
 * cf5cb7ef; la comparación de punta a punta es el DOL gx_tri.
 */
#include <string.h>

#include "gx/gx.h"
#include "gx/vtx.h"
#include "test.h"

/* VCD (CP 0x50 y 0x60): 0 no está, 1 directo, 2 índice de 8 bits, 3 de 16 */
#define VCD_POSMTX       1u
#define VCD_TEXMTX(i)    (1u << (1 + (i)))
#define VCD_POS(t)       ((uint32_t)(t) << 9)
#define VCD_NRM(t)       ((uint32_t)(t) << 11)
#define VCD_COL(i, t)    ((uint32_t)(t) << (13 + 2 * (i)))
#define VCD_TEX(i, t)    ((uint32_t)(t) << (2 * (i)))      /* VCD_HI */

/* VAT grupo A (CP 0x70) */
#define G0_POS(xyz, fmt, frac) ((uint32_t)(xyz) | (uint32_t)(fmt) << 1 | (uint32_t)(frac) << 4)
#define G0_NRM(ntb, fmt)       ((uint32_t)(ntb) << 9 | (uint32_t)(fmt) << 10)
#define G0_COL(i, fmt)         ((uint32_t)(fmt) << (14 + 4 * (i)))
#define G0_TEX0(st, fmt, frac) ((uint32_t)(st) << 21 | (uint32_t)(fmt) << 22 | (uint32_t)(frac) << 25)
#define G0_DEQUANT             (1u << 30)
#define G0_INDEX3              (1u << 31)

enum { NO = 0, DIR = 1, IX8 = 2, IX16 = 3 };
enum { U8 = 0, S8 = 1, U16 = 2, S16 = 3, F32 = 4 };
enum { C565 = 0, C888 = 1, C888X = 2, C4444 = 3, C6666 = 4, C8888 = 5 };

static uint8_t ram[0x1000];
static uint32_t base[16], stride[16];
static gx_vtx_defaults_t def;
static gx_vtx_plan_t plan;
static uint8_t gathered[GX_VTX_BLOCK * GX_VTX_DIRECT];
static gx_vtx_in_t out[GX_VTX_BLOCK];

static const uint8_t *ram_fn(void *ctx, uint32_t addr, uint32_t len)
{
    (void)ctx;
    if (addr > sizeof ram || len > sizeof ram - addr)
        return NULL;
    return ram + addr;
}

static void reset(void)
{
    memset(ram, 0, sizeof ram);
    memset(base, 0, sizeof base);
    memset(stride, 0, sizeof stride);
    memset(&def, 0, sizeof def);
    memset(out, 0xAB, sizeof out);
}

/* Arma el plan, junta y decodifica: devuelve cuántos vértices quedaron */
static uint32_t load(uint32_t lo, uint32_t hi, uint32_t g0, uint32_t g1, uint32_t g2,
                     const uint8_t *raw, uint32_t n, uint32_t *bad)
{
    const uint32_t vcd[2] = { lo, hi }, vat[3] = { g0, g1, g2 };
    gx_vtx_setup(vcd, vat, &plan);
    const uint32_t m = gx_vtx_gather(&plan, base, stride, ram_fn, NULL, raw, n, gathered, bad);
    gx_vtx_decode(&plan.desc, &def, gathered, m, out);
    return m;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static uint32_t rgba(const uint8_t c[4])
{
    return (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3];
}

static void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_bef(uint8_t *p, float f)
{
    const uint32_t u = fbits(f);
    p[0] = (uint8_t)(u >> 24);
    p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);
    p[3] = (uint8_t)u;
}

/* ---- tamaño ------------------------------------------------------------------------- */

/* El tamaño en el FIFO es el de CP (cpmem.c) para formatos al azar, y el
 * vértice juntado mide lo mismo que el mismo formato con todo directo. */
static void test_vtx_size_matches_cp(void)
{
    static gc_gx_t gx;
    uint32_t seed = 12345;
    for (int it = 0; it < 4000; it++) {
        uint32_t r[5];
        for (int k = 0; k < 5; k++) {
            seed = seed * 1664525u + 1013904223u;
            r[k] = seed;
        }
        uint32_t lo = r[0] & 0x1FFFFu, hi = r[1] & 0xFFFFu, g0 = r[2], g1 = r[3], g2 = r[4];
        /* Formatos de color válidos (0-5): los otros los rechaza Dolphin */
        g0 &= ~(7u << 14 | 7u << 18);
        g0 |= (r[0] >> 20) % 6u << 14 | (r[1] >> 20) % 6u << 18;

        memset(&gx, 0, sizeof gx);
        gx.cp.vcd_lo = lo;
        gx.cp.vcd_hi = hi;
        gx.cp.vat[3][0] = g0;
        gx.cp.vat[3][1] = g1;
        gx.cp.vat[3][2] = g2;
        const uint32_t vcd[2] = { lo, hi }, vat[3] = { g0, g1, g2 };
        gx_vtx_setup(vcd, vat, &plan);
        CHECK_EQ_U64(plan.raw_size, gc_gx_vertex_size(&gx, 3));
        CHECK(plan.desc.stride <= GX_VTX_DIRECT);

        /* El mismo formato con todo directo */
        uint32_t dlo = lo & 0x1FFu, dhi = 0;
        for (unsigned k = 0; k < 4; k++)
            if ((lo >> (9 + 2 * k)) & 3u)
                dlo |= 1u << (9 + 2 * k);
        for (unsigned k = 0; k < 8; k++)
            if ((hi >> (2 * k)) & 3u)
                dhi |= 1u << (2 * k);
        const uint32_t stride_idx = plan.desc.stride;
        const uint32_t dvcd[2] = { dlo, dhi };
        gx_vtx_setup(dvcd, vat, &plan);
        CHECK_EQ_U64(plan.raw_size, stride_idx);
        CHECK_EQ_U64(plan.desc.stride, stride_idx);
    }
}

/* ---- posición ---------------------------------------------------------------------- */

static void test_vtx_position_formats(void)
{
    reset();
    /* s16 XYZ, 2 bits de fracción, con ByteDequant */
    const uint8_t s16[6] = { 0xFF, 0xFD, 0x00, 0x08, 0x80, 0x00 };
    CHECK_EQ_U64(load(VCD_POS(DIR), 0, G0_POS(1, S16, 2) | G0_DEQUANT, 0, 0, s16, 1, NULL), 1);
    CHECK_EQ_U64(fbits(out[0].pos[0]), fbits(-0.75f));
    CHECK_EQ_U64(fbits(out[0].pos[1]), fbits(2.0f));
    CHECK_EQ_U64(fbits(out[0].pos[2]), fbits(-8192.0f));

    /* Sin ByteDequant la fracción no se aplica (el cargador x64) */
    load(VCD_POS(DIR), 0, G0_POS(1, S16, 2), 0, 0, s16, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].pos[0]), fbits(-3.0f));
    CHECK_EQ_U64(fbits(out[0].pos[2]), fbits(-32768.0f));

    /* u8 XY: z queda en 0 */
    const uint8_t u8[2] = { 200, 7 };
    load(VCD_POS(DIR), 0, G0_POS(0, U8, 3) | G0_DEQUANT, 0, 0, u8, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].pos[0]), fbits(25.0f));
    CHECK_EQ_U64(fbits(out[0].pos[1]), fbits(0.875f));
    CHECK_EQ_U64(fbits(out[0].pos[2]), 0);

    /* s8 y u16 */
    const uint8_t s8[3] = { 0x80, 0x7F, 0x01 };
    load(VCD_POS(DIR), 0, G0_POS(1, S8, 7) | G0_DEQUANT, 0, 0, s8, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].pos[0]), fbits(-1.0f));
    CHECK_EQ_U64(fbits(out[0].pos[1]), fbits(127.0f / 128.0f));
    const uint8_t u16[6] = { 0xFF, 0xFF, 0x00, 0x01, 0x80, 0x00 };
    load(VCD_POS(DIR), 0, G0_POS(1, U16, 16) | G0_DEQUANT, 0, 0, u16, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].pos[0]), fbits(65535.0f / 65536.0f));
    CHECK_EQ_U64(fbits(out[0].pos[2]), fbits(0.5f));

    /* float: los bits tal cual, la fracción no cuenta */
    uint8_t f[12];
    put_bef(f, 1.5f);
    put_bef(f + 4, -2.25f);
    put_bef(f + 8, 1e30f);
    load(VCD_POS(DIR), 0, G0_POS(1, F32, 5) | G0_DEQUANT, 0, 0, f, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].pos[0]), fbits(1.5f));
    CHECK_EQ_U64(fbits(out[0].pos[1]), fbits(-2.25f));
    CHECK_EQ_U64(fbits(out[0].pos[2]), fbits(1e30f));

    /* Índice de 16 bits: el arreglo 0 en la RAM */
    base[0] = 0x200;
    stride[0] = 12;
    put_bef(ram + 0x200 + 12 * 3, 4.0f);
    put_bef(ram + 0x200 + 12 * 3 + 4, 5.0f);
    put_bef(ram + 0x200 + 12 * 3 + 8, 6.0f);
    const uint8_t ix[2] = { 0x00, 0x03 };
    CHECK_EQ_U64(load(VCD_POS(IX16), 0, G0_POS(1, F32, 0), 0, 0, ix, 1, NULL), 1);
    CHECK_EQ_U64(fbits(out[0].pos[0]), fbits(4.0f));
    CHECK_EQ_U64(fbits(out[0].pos[2]), fbits(6.0f));
}

/* ---- normales ---------------------------------------------------------------------- */

static void test_vtx_normal_scale(void)
{
    reset();
    /* La escala de las normales enteras no depende de ByteDequant */
    load(VCD_POS(DIR) | VCD_NRM(DIR), 0, G0_POS(0, U8, 0) | G0_NRM(0, S8), 0, 0,
         (const uint8_t[]){ 0, 0, 64, 0xC0, 0 }, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].nrm[0][0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][1]), fbits(-1.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][2]), 0);

    load(VCD_NRM(DIR), 0, G0_NRM(0, U8), 0, 0, (const uint8_t[]){ 128, 255, 1 }, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].nrm[0][0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][1]), fbits(255.0f / 128.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][2]), fbits(1.0f / 128.0f));

    load(VCD_NRM(DIR), 0, G0_NRM(0, S16), 0, 0, (const uint8_t[]){ 0x40, 0, 0xC0, 0, 0x20, 0 }, 1,
         NULL);
    CHECK_EQ_U64(fbits(out[0].nrm[0][0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][1]), fbits(-1.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][2]), fbits(0.5f));

    load(VCD_NRM(DIR), 0, G0_NRM(0, U16) | G0_DEQUANT, 0, 0,
         (const uint8_t[]){ 0x80, 0, 0xFF, 0xFF, 0, 1 }, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].nrm[0][0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][1]), fbits(65535.0f / 32768.0f));

    uint8_t f[12];
    put_bef(f, 0.25f);
    put_bef(f + 4, -3.0f);
    put_bef(f + 8, 7.0f);
    load(VCD_NRM(DIR), 0, G0_NRM(0, F32), 0, 0, f, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].nrm[0][0]), fbits(0.25f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][2]), fbits(7.0f));
}

/* NTB: directo, con un índice y con tres (NormalIndex3) */
static void test_vtx_ntb_index3(void)
{
    reset();
    uint8_t d[18];
    for (int k = 0; k < 9; k++)
        put_be16(d + 2 * k, (uint32_t)(k + 1) * 0x800u);              /* (k+1)/8 con la escala 2^-14 */
    load(VCD_NRM(DIR), 0, G0_NRM(1, S16) | G0_INDEX3, 0, 0, d, 1, NULL);
    CHECK_EQ_U64(plan.raw_size, 18);
    for (int k = 0; k < 9; k++)
        CHECK_EQ_U64(fbits(out[0].nrm[k / 3][k % 3]), fbits((float)(k + 1) / 8.0f));
    CHECK(plan.has_nrm && plan.has_ntb);

    /* Entradas de 18 bytes con valores distintos: entrada e, componente c = e*16 + c */
    base[1] = 0x300;
    stride[1] = 20;
    for (int e = 0; e < 8; e++)
        for (int c = 0; c < 9; c++)
            put_be16(ram + 0x300 + 20 * e + 2 * c, (uint32_t)(e * 16 + c) * 64u);

    /* Un índice: los 9 componentes de la misma entrada */
    load(VCD_NRM(IX8), 0, G0_NRM(1, S16), 0, 0, (const uint8_t[]){ 2 }, 1, NULL);
    CHECK_EQ_U64(plan.raw_size, 1);
    for (int c = 0; c < 9; c++)
        CHECK_EQ_U64(fbits(out[0].nrm[c / 3][c % 3]), fbits((float)((32 + c) * 64) / 16384.0f));

    /* Tres índices: la normal de la entrada 1, la tangente de la 4 y la binormal de
     * la 6, cada una en su tercio (el desplazamiento se suma igual) */
    load(VCD_NRM(IX16), 0, G0_NRM(1, S16) | G0_INDEX3, 0, 0,
         (const uint8_t[]){ 0, 1, 0, 4, 0, 6 }, 1, NULL);
    CHECK_EQ_U64(plan.raw_size, 6);
    const int ent[3] = { 1, 4, 6 };
    for (int c = 0; c < 9; c++)
        CHECK_EQ_U64(fbits(out[0].nrm[c / 3][c % 3]), fbits((float)((ent[c / 3] * 16 + c) * 64) / 16384.0f));

    /* Directo con NormalIndex3: no hay índices, 9 componentes seguidos */
    gx_vtx_setup((const uint32_t[]){ VCD_NRM(DIR), 0 }, (const uint32_t[]){ G0_NRM(1, U8) | G0_INDEX3, 0, 0 },
                 &plan);
    CHECK_EQ_U64(plan.raw_size, 9);
}

/* ---- colores ------------------------------------------------------------------------ */

static void test_vtx_colors(void)
{
    static const struct { uint8_t fmt, n, raw[4]; uint32_t want; } c[] = {
        { C565, 2, { 0x84, 0x10 }, 0x848284FFu },
        { C565, 2, { 0xF8, 0x1F }, 0xFF00FFFFu },
        { C888, 3, { 0x12, 0x34, 0x56 }, 0x123456FFu },
        { C888X, 4, { 0x12, 0x34, 0x56, 0x00 }, 0x123456FFu },
        { C4444, 2, { 0x12, 0x34 }, 0x11223344u },
        { C6666, 3, { 0xFC, 0x05, 0x6A }, 0xFF0055AAu },
        { C8888, 4, { 1, 2, 3, 4 }, 0x01020304u },
    };
    reset();
    for (size_t k = 0; k < sizeof c / sizeof c[0]; k++) {
        load(VCD_COL(0, DIR), 0, G0_COL(0, c[k].fmt), 0, 0, c[k].raw, 1, NULL);
        CHECK_EQ_U64(plan.raw_size, c[k].n);
        CHECK_EQ_U64(rgba(out[0].col[0]), c[k].want);
        CHECK_EQ_U64(rgba(out[0].col[1]), 0xFFFFFFFFu);
    }

    /* Los dos: el 0 primero */
    load(VCD_COL(0, DIR) | VCD_COL(1, DIR), 0, G0_COL(0, C565) | G0_COL(1, C8888), 0, 0,
         (const uint8_t[]){ 0x84, 0x10, 9, 8, 7, 6 }, 1, NULL);
    CHECK_EQ_U64(rgba(out[0].col[0]), 0x848284FFu);
    CHECK_EQ_U64(rgba(out[0].col[1]), 0x09080706u);

    /* Solo el 1: va al canal 0 y el 1 queda en blanco */
    load(VCD_COL(1, DIR), 0, G0_COL(1, C4444), 0, 0, (const uint8_t[]){ 0xAB, 0xCD }, 1, NULL);
    CHECK_EQ_U64(rgba(out[0].col[0]), 0xAABBCCDDu);
    CHECK_EQ_U64(rgba(out[0].col[1]), 0xFFFFFFFFu);

    /* Ninguno */
    load(VCD_POS(DIR), 0, G0_POS(0, U8, 0), 0, 0, (const uint8_t[]){ 1, 2 }, 1, NULL);
    CHECK_EQ_U64(rgba(out[0].col[0]), 0xFFFFFFFFu);
    CHECK_EQ_U64(rgba(out[0].col[1]), 0xFFFFFFFFu);

    /* Indexado: arreglo 2 para el color 0, 3 para el color 1 */
    base[2] = 0x100;
    stride[2] = 4;
    base[3] = 0x180;
    stride[3] = 2;
    memcpy(ram + 0x100 + 4 * 5, (const uint8_t[]){ 0xDE, 0xAD, 0xBE, 0xEF }, 4);
    memcpy(ram + 0x180 + 2 * 0x102, (const uint8_t[]){ 0x00, 0x1F }, 2);
    load(VCD_COL(0, IX8) | VCD_COL(1, IX16), 0, G0_COL(0, C8888) | G0_COL(1, C565), 0, 0,
         (const uint8_t[]){ 5, 0x01, 0x02 }, 1, NULL);
    CHECK_EQ_U64(rgba(out[0].col[0]), 0xDEADBEEFu);
    CHECK_EQ_U64(rgba(out[0].col[1]), 0x0000FFFFu);
}

/* ---- coordenadas y matrices -------------------------------------------------------- */

static void test_vtx_texcoords_and_matrices(void)
{
    reset();
    def.posmtx = 7;
    for (int i = 0; i < 8; i++)
        def.texmtx[i] = (uint8_t)(30 + i);

    /* posmtx: 6 bits; texmtx: el byte entero; los que faltan, de XF */
    const uint8_t raw[] = { 0xC5, 0xFA, 9 };
    load(VCD_POSMTX | VCD_TEXMTX(1), VCD_TEX(0, DIR), G0_TEX0(0, U8, 1) | G0_DEQUANT, 0, 0, raw, 1,
         NULL);
    CHECK_EQ_U64(out[0].posmtx, 0x05);
    CHECK_EQ_U64(out[0].texmtx[0], 30);
    CHECK_EQ_U64(out[0].texmtx[1], 0xFA);
    CHECK_EQ_U64(out[0].texmtx[7], 37);
    /* S sola: t en 0 */
    CHECK_EQ_U64(fbits(out[0].tex[0][0]), fbits(4.5f));
    CHECK_EQ_U64(fbits(out[0].tex[0][1]), 0);

    load(0, 0, 0, 0, 0, NULL, 1, NULL);
    CHECK_EQ_U64(out[0].posmtx, 7);
    CHECK_EQ_U64(out[0].texmtx[1], 31);

    /* Tex4: la fracción está en el grupo C (bits 0-4); tex7 al final del grupo C */
    const uint32_t g1 = 1u << 27 | (uint32_t)S16 << 28;
    const uint32_t g2 = 4u | 1u << 23 | (uint32_t)U16 << 24 | 8u << 27;
    load(0, VCD_TEX(4, DIR) | VCD_TEX(7, DIR), G0_DEQUANT, g1, g2,
         (const uint8_t[]){ 0x00, 0x10, 0xFF, 0xF0, 0x01, 0x00, 0x00, 0x80 }, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].tex[4][0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].tex[4][1]), fbits(-1.0f));
    CHECK_EQ_U64(fbits(out[0].tex[7][0]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].tex[7][1]), fbits(0.5f));

    /* Sin ByteDequant: los enteros tal cual */
    load(0, VCD_TEX(4, DIR), 0, g1, g2, (const uint8_t[]){ 0x00, 0x10, 0xFF, 0xF0 }, 1, NULL);
    CHECK_EQ_U64(fbits(out[0].tex[4][0]), fbits(16.0f));
    CHECK_EQ_U64(fbits(out[0].tex[4][1]), fbits(-16.0f));

    /* Orden en el FIFO: posmtx, texmtx, posición, normal, colores, coordenadas */
    const uint8_t all[] = { 3, 4, 10, 20, 64, 64, 64, 0x11, 0x22, 0x33, 5 };
    load(VCD_POSMTX | VCD_TEXMTX(2) | VCD_POS(DIR) | VCD_NRM(DIR) | VCD_COL(0, DIR),
         VCD_TEX(2, DIR), G0_POS(0, U8, 0) | G0_NRM(0, U8) | G0_COL(0, C888), 0, 0, all, 1, NULL);
    CHECK_EQ_U64(plan.raw_size, sizeof all);
    CHECK_EQ_U64(out[0].posmtx, 3);
    CHECK_EQ_U64(out[0].texmtx[2], 4);
    CHECK_EQ_U64(fbits(out[0].pos[1]), fbits(20.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[0][2]), fbits(0.5f));
    CHECK_EQ_U64(rgba(out[0].col[0]), 0x112233FFu);
    CHECK_EQ_U64(fbits(out[0].tex[2][0]), fbits(5.0f));
}

/* ---- descartes, lecturas fuera de la RAM y la caché de normales -------------------- */

static void test_vtx_skip(void)
{
    reset();
    base[0] = 0x40;
    stride[0] = 2;
    base[1] = 0x80;
    stride[1] = 3;
    for (int e = 0; e < 4; e++) {
        ram[0x40 + 2 * e] = (uint8_t)(10 + e);
        ram[0x40 + 2 * e + 1] = (uint8_t)(20 + e);
    }
    /* Posición con índice 0xFF: el vértice no queda */
    uint32_t bad = 0;
    CHECK_EQ_U64(load(VCD_POS(IX8), 0, G0_POS(0, U8, 0), 0, 0,
                      (const uint8_t[]){ 0, 0xFF, 1, 2 }, 4, &bad), 3);
    CHECK_EQ_U64(fbits(out[0].pos[0]), fbits(10.0f));
    CHECK_EQ_U64(fbits(out[1].pos[0]), fbits(11.0f));
    CHECK_EQ_U64(fbits(out[2].pos[1]), fbits(22.0f));
    CHECK_EQ_U64(bad, 0);

    CHECK_EQ_U64(load(VCD_POS(IX16), 0, G0_POS(0, U8, 0), 0, 0,
                      (const uint8_t[]){ 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF }, 3, NULL), 1);

    /* Una normal con índice 0xFF no descarta: lee la entrada 255 */
    ram[0x80 + 3 * 255] = 128;
    CHECK_EQ_U64(load(VCD_POS(IX8) | VCD_NRM(IX8), 0, G0_POS(0, U8, 0) | G0_NRM(0, U8), 0, 0,
                      (const uint8_t[]){ 1, 0xFF }, 1, NULL), 1);
    CHECK_EQ_U64(fbits(out[0].nrm[0][0]), fbits(1.0f));
}

static void test_vtx_bad_reads(void)
{
    reset();
    base[0] = 0xFF8;                                /* 12 bytes desde ahí ya no están */
    stride[0] = 0x100;
    uint32_t bad = 0;
    CHECK_EQ_U64(load(VCD_POS(IX8), 0, G0_POS(1, F32, 0), 0, 0, (const uint8_t[]){ 0, 1 }, 2, &bad),
                 2);
    CHECK_EQ_U64(bad, 2);
    CHECK_EQ_U64(fbits(out[0].pos[1]), 0);
    CHECK_EQ_U64(fbits(out[1].pos[0]), 0);
    CHECK_EQ_U64(fbits(out[1].pos[2]), 0);
}

static void test_vtx_normal_defaults(void)
{
    reset();
    for (int k = 0; k < 9; k++)
        def.nrm[k / 3][k % 3] = (float)(k + 1);

    /* Sin normal: las tres de la caché */
    load(VCD_POS(DIR), 0, G0_POS(0, U8, 0), 0, 0, (const uint8_t[]){ 1, 2 }, 1, NULL);
    CHECK(!plan.has_nrm && !plan.has_ntb);
    for (int k = 0; k < 9; k++)
        CHECK_EQ_U64(fbits(out[0].nrm[k / 3][k % 3]), fbits((float)(k + 1)));

    /* Solo N: tangente y binormal de la caché */
    load(VCD_NRM(DIR), 0, G0_NRM(0, S8), 0, 0, (const uint8_t[]){ 0, 0, 64 }, 1, NULL);
    CHECK(plan.has_nrm && !plan.has_ntb);
    CHECK_EQ_U64(fbits(out[0].nrm[0][0]), 0);
    CHECK_EQ_U64(fbits(out[0].nrm[0][2]), fbits(1.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[1][0]), fbits(4.0f));
    CHECK_EQ_U64(fbits(out[0].nrm[2][2]), fbits(9.0f));
}

/* Un bloque entero: cada vértice con su dato */
static void test_vtx_block(void)
{
    reset();
    uint8_t raw[GX_VTX_BLOCK * 8];
    for (int v = 0; v < GX_VTX_BLOCK; v++) {
        put_be16(raw + 8 * v, (uint32_t)(v * 3));
        put_be16(raw + 8 * v + 2, (uint32_t)(-v) & 0xFFFFu);
        raw[8 * v + 4] = (uint8_t)v;
        raw[8 * v + 5] = (uint8_t)(255 - v);
        raw[8 * v + 6] = (uint8_t)(v ^ 0x55);
        raw[8 * v + 7] = 7;
    }
    CHECK_EQ_U64(load(VCD_POS(DIR) | VCD_COL(0, DIR), 0, G0_POS(0, S16, 1) | G0_DEQUANT | G0_COL(0, C8888),
                      0, 0, raw, GX_VTX_BLOCK, NULL),
                 GX_VTX_BLOCK);
    for (int v = 0; v < GX_VTX_BLOCK; v++) {
        CHECK_EQ_U64(fbits(out[v].pos[0]), fbits((float)(v * 3) / 2.0f));
        CHECK_EQ_U64(fbits(out[v].pos[1]), fbits((float)-v / 2.0f));
        CHECK_EQ_U64(rgba(out[v].col[0]),
                     (uint32_t)v << 24 | (uint32_t)(255 - v) << 16 | (uint32_t)(v ^ 0x55) << 8 | 7u);
    }
}

const test_case_t vtx_tests[] = {
    { "vtx: tamaño igual al de CP", test_vtx_size_matches_cp },
    { "vtx: posición en todos los formatos", test_vtx_position_formats },
    { "vtx: escala de las normales", test_vtx_normal_scale },
    { "vtx: NTB con uno y tres índices", test_vtx_ntb_index3 },
    { "vtx: colores", test_vtx_colors },
    { "vtx: coordenadas y matrices", test_vtx_texcoords_and_matrices },
    { "vtx: descarte por índice de posición", test_vtx_skip },
    { "vtx: lecturas fuera de la RAM", test_vtx_bad_reads },
    { "vtx: normales de la caché", test_vtx_normal_defaults },
    { "vtx: bloque de 64", test_vtx_block },
    { NULL, NULL },
};
