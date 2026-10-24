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
 * DOL de prueba "gx_tex" (Fase 2, etapa C4): texturas con la mini-librería
 * propia (gxlib y gxvtx, sin libogc). Las texturas son bytes al azar (con una
 * semilla fija): cualquier byte es un texel válido en todos los formatos.
 *
 *   1. los 11 formatos, los 3 con paleta en las 3 paletas (cargada a la TMEM
 *      por BP 0x64 y 0x65), y tamaños que no son múltiplos del bloque;
 *   2. envolturas: las 9 combinaciones de recortar, repetir y espejo, y sin
 *      potencia de 2;
 *   3. filtros: más cercano y bilineal al agrandar y al achicar;
 *   4. mipmaps: sin, más cercano y lineal, sesgo, LOD mínimo y máximo, LOD
 *      diagonal y un piso en perspectiva;
 *   5. TMEM: texturas precargadas (BP 0x60-0x63), RGBA8 partida en dos
 *      bancos, también con mipmaps, y una con paleta;
 *   6. unidades de textura 1 a 7;
 *   7. textura por color del vértice y mezcla con su alfa.
 *
 * Se compara contra el renderizador por software de Dolphin: cada triángulo y
 * el CRC del EFB en copias sin destino al final de cada parte
 * (tests/dol/dolphin/gx_tex.cop.txt).
 *
 * Solo para el DOL: no compila en la PC.
 */
#include <stddef.h>

#include "gxlib.h"
#include "rt.h"

#define FIFO_SIZE (64u * 1024u)
#define MAX_WAIT  2000000u

static uint8_t fifo[FIFO_SIZE] __attribute__((aligned(32)));
static uint8_t pool[96u * 1024u] __attribute__((aligned(32)));
static uint32_t pool_used;

static uint32_t token;
static int fails;

enum {
    BP_TEVORDER = 0x28, BP_ZMODE = 0x40, BP_BLEND = 0x41, BP_ZCONTROL = 0x43, BP_COPY_XY = 0x49,
    BP_COPY_WH = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D, BP_CLEAR_AR = 0x4F, BP_CLEAR_GB = 0x50,
    BP_CLEAR_Z = 0x51, BP_COPY = 0x52, BP_SCISSOR_OFFSET = 0x59, BP_PRELOAD_ADDR = 0x60,
    BP_PRELOAD_EVEN = 0x61, BP_PRELOAD_ODD = 0x62, BP_PRELOAD_MODE = 0x63, BP_TLUT_SRC = 0x64,
    BP_TLUT_DEST = 0x65, BP_TEXINVALIDATE = 0x66, BP_COLOR_C0 = 0xC0, BP_ALPHA_C0 = 0xC1,
    BP_ALPHATEST = 0xF3, BP_KSEL = 0xF6,
};
enum {
    TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6, TF_C4 = 8,
    TF_C8 = 9, TF_C14X2 = 10, TF_CMPR = 14,
};
enum { WRAP_CLAMP = 0, WRAP_REPEAT = 1, WRAP_MIRROR = 2 };
enum { MIP_NONE = 0, MIP_POINT = 1, MIP_LINEAR = 2 };

/* TexMode0: envolturas, filtros, mipmaps, LOD diagonal y sesgo en s2.5 */
#define MODE0(ws, wt, mag, mip, min, diag, bias)                                                   \
    ((uint32_t)(ws) | (uint32_t)(wt) << 2 | (uint32_t)(mag) << 4 | (uint32_t)(mip) << 5 |          \
     (uint32_t)(min) << 7 | (uint32_t)(diag) << 8 | ((uint32_t)(bias) & 0xFFu) << 9)
/* TexMode1: LOD mínimo y máximo en s?.4 */
#define MODE1(minlod, maxlod) ((uint32_t)(minlod) | (uint32_t)(maxlod) << 8)
#define CC(a, b, c, d) ((uint32_t)(a) << 12 | (uint32_t)(b) << 8 | (uint32_t)(c) << 4 | (uint32_t)(d) | 1u << 19)
#define AC(a, b, c, d) ((uint32_t)(a) << 13 | (uint32_t)(b) << 10 | (uint32_t)(c) << 7 | (uint32_t)(d) << 4 | 1u << 19)

static const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
/* Ortográfica en píxeles exactos (como gx_raster): x de la pantalla = x + 342 */
static const float ortho_px[6] = { 1.0f / 512.0f, -1.0f, -1.0f / 512.0f, 1.0f, 1.0f / 16777216.0f, -1.0f };
static const float viewport_px[6] = { 512.0f, -512.0f, 16777216.0f, 854.0f, 854.0f, 16777216.0f };
static const float persp[6] = { 1.2990381f, 0, 1.7320508f, 0, -0.001001001f, -1.001001f };
static const float viewport_persp[6] = { 320.0f, -240.0f, 16777215.0f, 662.0f, 582.0f, 16777215.0f };

/* ---- utilidades -------------------------------------------------------------------- */

static void sync_gpu(const char *what)
{
    token++;
    gx_bp(BP_TOKEN, token);
    gx_flush();
    if (!gx_wait_idle(MAX_WAIT)) {
        OSReport("gx_tex: %s: la GPU no vacio el FIFO\n", what);
        fails++;
        return;
    }
    uint32_t n = 0;
    while (PE_REG(PE_TOKEN) != (uint16_t)token && n < MAX_WAIT)
        n++;
    if (n == MAX_WAIT) {
        OSReport("gx_tex: %s: el token %u no llego\n", what, (unsigned)token);
        fails++;
        return;
    }
    OSReport("gx_tex: %s\n", what);
}

static uint32_t seed = 0x2545F491u;

static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

/* Bytes al azar en el área de texturas, alineados a 32, ya en la RAM */
static uint8_t *random_bytes(uint32_t n)
{
    uint8_t *p = pool + pool_used;
    pool_used += (n + 31u) & ~31u;
    if (pool_used > sizeof pool) {
        OSReport("gx_tex: sin lugar para las texturas\n");
        fails++;
        exit(1);
    }
    for (uint32_t i = 0; i < n; i++)
        p[i] = (uint8_t)rnd();
    dc_flush(p, n);
    return p;
}

/* Bytes de una textura (con el relleno del bloque) */
static uint32_t tex_bytes(uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t bw = 8, bh = 4, bpb = 32;
    if (fmt == TF_I4 || fmt == TF_C4 || fmt == TF_CMPR)
        bh = 8;
    else if (fmt == TF_IA8 || fmt == TF_RGB565 || fmt == TF_RGB5A3 || fmt == TF_C14X2)
        bw = 4;
    else if (fmt == TF_RGBA8) {
        bw = 4;
        bpb = 64;
    }
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb;
}

static void genmode(uint32_t texgens, uint32_t chans, uint32_t stages)
{
    gx_bp(BP_GENMODE, texgens | chans << 4 | (stages - 1) << 10);
    gx_xf1(XF_NUMTEXGEN, texgens);
    gx_xf1(XF_NUMCHAN, chans);
}

/* La unidad de textura map: modos, tamaño, formato, datos en la RAM y paleta */
static void tex_unit(uint32_t map, uint32_t mode0, uint32_t mode1, uint32_t w, uint32_t h, uint32_t fmt,
                     const void *data, uint32_t tlut_line, uint32_t tlutfmt)
{
    const uint32_t r = 0x80u | (map & 3u) | (map & 4u) << 3;
    gx_bp(r + 0x00, mode0);
    gx_bp(r + 0x04, mode1);
    gx_bp(r + 0x08, (w - 1) | (h - 1) << 10 | fmt << 20);
    gx_bp(r + 0x0C, 0);                         /* desde la RAM */
    gx_bp(r + 0x14, data ? gx_phys(data) >> 5 : 0);
    gx_bp(r + 0x18, tlut_line | tlutfmt << 10);
}

/* La misma unidad leyendo de la TMEM (líneas de 32 bytes, par e impar) */
static void tex_unit_tmem(uint32_t map, uint32_t even, uint32_t odd)
{
    const uint32_t r = 0x80u | (map & 3u) | (map & 4u) << 3;
    gx_bp(r + 0x0C, even | 1u << 21);
    gx_bp(r + 0x10, odd);
}

/* Etapa 0 con la textura map y la coordenada 0, y SU con el tamaño */
static void use_map(uint32_t map, uint32_t w, uint32_t h)
{
    gx_bp(BP_TEVORDER, map | 1u << 6);
    gx_bp(BP_SU_SSIZE, w - 1);
    gx_bp(BP_SU_TSIZE, h - 1);
}

static void v(float x, float y, float z, uint32_t c, float s, float t)
{
    gx_f32(x);
    gx_f32(y);
    gx_f32(z);
    gx_u32(c);
    gx_f32(s);
    gx_f32(t);
}

/* Un rectángulo con coordenadas normalizadas de (s0, t0) a (s1, t1) */
static void tquad(float x, float y, float w, float h, float s0, float t0, float s1, float t1)
{
    gx_begin(GX_QUADS, 0, 4);
    v(x, y, 0, 0xFFFFFFFFu, s0, t0);
    v(x + w, y, 0, 0xFFFFFFFFu, s1, t0);
    v(x + w, y + h, 0, 0xFFFFFFFFu, s1, t1);
    v(x, y + h, 0, 0xFFFFFFFFu, s0, t1);
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
}

static void clear_efb(uint32_t rgba)
{
    gx_bp(BP_CLEAR_AR, (rgba & 0xFFu) << 8 | rgba >> 24);
    gx_bp(BP_CLEAR_GB, (rgba >> 8) & 0xFFFFu);
    gx_bp(BP_CLEAR_Z, 0xFFFFFF);
    copy_efb(0, 0, 640, 528, 1u << 11);
}

/* ---- 0. estado inicial -------------------------------------------------------------- */

static uint8_t *tlut_ram;

static void setup(void)
{
    gx_init_fifo(fifo, FIFO_SIZE, FIFO_SIZE - 16384, FIFO_SIZE / 2, CP_CR_READ | CP_CR_LINK);
    genmode(1, 1, 1);
    gx_xf1(XF_DUALTEX, 0);
    gx_projection(ortho_px, 1);
    gx_xf_floats(XF_VIEWPORT, 6, viewport_px);
    gx_pos_matrix(0, ident);
    gx_pos_matrix(30, ident);
    gx_matindex(0 | 30u << 6, 0);
    gx_xf1(XF_COLOR0, CH_MATSRC_VTX);
    gx_xf1(XF_ALPHA0, CH_MATSRC_VTX);
    gx_xf1(XF_TEXMTXINFO, TG_ROW(5));
    gx_vat(0, VAT_POS(1, FMT_F32, 0) | VAT_COL0(COL_8888) | VAT_TEX0(1, FMT_F32, 0), 0, 0);
    gx_vtxdesc(VCD_POS(VTX_DIRECT) | VCD_COL0(VTX_DIRECT), VCD_TEX(0, VTX_DIRECT), 0);

    gx_bp(0x20, 342 | 342u << 12);              /* tijera en todo el EFB */
    gx_bp(0x21, (342 + 527) | (342u + 639) << 12);
    gx_bp(BP_SCISSOR_OFFSET, 171 | 171u << 10);
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 8));      /* PREV = tex.rgb */
    gx_bp(BP_ALPHA_C0, AC(7, 7, 7, 4));         /* PREV = tex.a */
    gx_bp(BP_KSEL, 1u << 2);                    /* swap 0: r g b a */
    gx_bp(BP_KSEL + 1, 2u | 3u << 2);
    gx_bp(BP_ALPHATEST, 7u << 16 | 7u << 19);
    gx_bp(BP_ZCONTROL, 1);                      /* RGBA6: el alfa de la textura queda */
    gx_bp(BP_ZMODE, 0);
    gx_bp(BP_BLEND, 1u << 3 | 1u << 4);

    /* Una paleta de 16384 entradas al azar, a la TMEM desde la línea 512 × 16 */
    tlut_ram = random_bytes(32768);
    gx_bp(BP_TLUT_SRC, gx_phys(tlut_ram) >> 5);
    gx_bp(BP_TLUT_DEST, 512 | 1024u << 10);
    sync_gpu("estado inicial");
}

/* ---- 1. formatos -------------------------------------------------------------------- */

static void formats(void)
{
    static const uint32_t direct[8] = { TF_I4, TF_I8, TF_IA4, TF_IA8, TF_RGB565, TF_RGB5A3, TF_RGBA8, TF_CMPR };
    static const uint32_t paletted[3] = { TF_C4, TF_C8, TF_C14X2 };
    clear_efb(0x204060FFu);
    uint32_t k = 0;
    for (uint32_t i = 0; i < 8; i++, k++) {
        const uint8_t *t = random_bytes(tex_bytes(direct[i], 16, 16));
        tex_unit(0, MODE0(0, 0, 0, 0, 0, 0, 0), 0, 16, 16, direct[i], t, 0, 0);
        use_map(0, 16, 16);
        tquad(4 + 36 * (float)(k % 16), 4 + 36 * (float)(k / 16), 32, 32, 0, 0, 1, 1);
    }
    for (uint32_t i = 0; i < 3; i++)
        for (uint32_t f = 0; f < 3; f++, k++) {
            const uint8_t *t = random_bytes(tex_bytes(paletted[i], 16, 16));
            tex_unit(0, 0, 0, 16, 16, paletted[i], t, 512, f);
            use_map(0, 16, 16);
            tquad(4 + 36 * (float)(k % 16), 4 + 36 * (float)(k / 16), 32, 32, 0, 0, 1, 1);
        }
    /* Tamaños que no llenan el bloque, 1 a 1 */
    static const uint32_t odd[6][3] = {
        { TF_I4, 12, 10 }, { TF_I8, 9, 3 }, { TF_RGB5A3, 7, 5 }, { TF_RGBA8, 6, 5 }, { TF_CMPR, 12, 12 },
        { TF_C8, 10, 6 },
    };
    for (uint32_t i = 0; i < 6; i++) {
        const uint8_t *t = random_bytes(tex_bytes(odd[i][0], odd[i][1], odd[i][2]));
        tex_unit(0, 0, 0, odd[i][1], odd[i][2], odd[i][0], t, 512, 2);
        use_map(0, odd[i][1], odd[i][2]);
        tquad(4 + 20 * (float)i, 84, (float)odd[i][1], (float)odd[i][2], 0, 0, 1, 1);
    }
    snap(0, 0, 640, 100);
    sync_gpu("1. formatos");
}

/* ---- 2. envolturas ------------------------------------------------------------------ */

static void wraps(void)
{
    clear_efb(0x000000FFu);
    const uint8_t *t = random_bytes(tex_bytes(TF_RGB565, 16, 16));
    for (uint32_t ws = 0; ws < 3; ws++)
        for (uint32_t wt = 0; wt < 3; wt++) {
            tex_unit(0, MODE0(ws, wt, 0, 0, 0, 0, 0), 0, 16, 16, TF_RGB565, t, 0, 0);
            use_map(0, 16, 16);
            tquad(4 + 68 * (float)(ws * 3 + wt), 4, 64, 64, -1.5f, -1.25f, 2.5f, 2.75f);
        }
    /* Sin potencia de 2: repetir hace AND con el tamaño - 1 */
    const uint8_t *n = random_bytes(tex_bytes(TF_I8, 12, 10));
    for (uint32_t m = 1; m < 3; m++) {
        tex_unit(0, MODE0(m, m, 1, 0, 1, 0, 0), 0, 12, 10, TF_I8, n, 0, 0);
        use_map(0, 12, 10);
        tquad(4 + 100 * (float)(m - 1), 80, 96, 80, -2.0f, -1.0f, 3.0f, 3.0f);
    }
    snap(0, 0, 640, 170);
    sync_gpu("2. envolturas");
}

/* ---- 3. filtros --------------------------------------------------------------------- */

static void filters(void)
{
    clear_efb(0x000000FFu);
    const uint8_t *t = random_bytes(tex_bytes(TF_RGBA8, 8, 8));
    /* Agrandar 8 veces: más cercano y bilineal, con medio texel corrido */
    for (uint32_t mag = 0; mag < 2; mag++) {
        tex_unit(0, MODE0(WRAP_REPEAT, WRAP_REPEAT, mag, 0, 0, 0, 0), 0, 8, 8, TF_RGBA8, t, 0, 0);
        use_map(0, 8, 8);
        tquad(4 + 70 * (float)mag, 4, 64, 64, 0, 0, 1, 1);
        tquad(144 + 70 * (float)mag, 4, 64, 64, 0.0625f, 0.03125f, 1.0625f, 1.03125f);
    }
    /* Achicar 4 veces sin mipmaps: el filtro de achicar */
    const uint8_t *big = random_bytes(tex_bytes(TF_RGB565, 64, 64));
    for (uint32_t min = 0; min < 2; min++)
        for (uint32_t mag = 0; mag < 2; mag++) {
            tex_unit(0, MODE0(WRAP_CLAMP, WRAP_CLAMP, mag, 0, min, 0, 0), 0, 64, 64, TF_RGB565, big, 0, 0);
            use_map(0, 64, 64);
            tquad(4 + 20 * (float)(min * 2 + mag), 80, 16, 16, 0, 0, 1, 1);
            tquad(100 + 36 * (float)(min * 2 + mag), 80, 33, 31, 0, 0, 1, 1);   /* casi 1 a 2 */
        }
    snap(0, 0, 640, 120);
    sync_gpu("3. filtros");
}

/* ---- 4. mipmaps --------------------------------------------------------------------- */

static void mipmaps(void)
{
    clear_efb(0x000000FFu);
    /* 32 × 32 RGB5A3 y sus mipmaps hasta 1 × 1, uno detrás del otro (como los
     * cuenta Dolphin, con mínimo de un bloque) */
    const uint8_t *t = random_bytes(2048 + 512 + 128 + 32 + 32 + 32);
    uint32_t k = 0;
    for (uint32_t mip = 0; mip < 3; mip++)
        for (uint32_t min = 0; min < 2; min++, k++) {
            tex_unit(0, MODE0(WRAP_REPEAT, WRAP_REPEAT, 1, mip, min, 0, 0), MODE1(0, 0xA0), 32, 32,
                     TF_RGB5A3, t, 0, 0);
            use_map(0, 32, 32);
            for (uint32_t z = 0; z < 5; z++) {
                const float sz = (float)(32 >> z) * 0.75f + 2;
                tquad(4 + 40 * (float)z, 4 + 40 * (float)k, sz, sz, 0, 0, 1, 1);
            }
        }
    /* Sesgo (-2 a +1,5), LOD mínimo y máximo, LOD diagonal */
    static const int32_t bias[4] = { -64, -16, 16, 48 };
    for (uint32_t b = 0; b < 4; b++) {
        tex_unit(0, MODE0(WRAP_REPEAT, WRAP_REPEAT, 1, MIP_LINEAR, 1, 0, bias[b]), MODE1(0, 0xA0), 32, 32,
                 TF_RGB5A3, t, 0, 0);
        use_map(0, 32, 32);
        tquad(210 + 20 * (float)b, 4, 10, 10, 0, 0, 1, 1);
    }
    /* LOD mínimo 1,5; máximo 1; mínimo 3 y máximo 1 a la vez (gana el máximo
     * si se pasa); diagonal sin máximo */
    static const uint32_t mode1[4] = { MODE1(0x18, 0xA0), MODE1(0, 0x10), MODE1(0x30, 0x10), MODE1(0, 0xA0) };
    for (uint32_t l = 0; l < 4; l++) {
        tex_unit(0, MODE0(WRAP_REPEAT, WRAP_REPEAT, 1, l == 3 ? MIP_LINEAR : MIP_POINT, 1, l == 3, 0), mode1[l], 32, 32,
                 TF_RGB5A3, t, 0, 0);
        use_map(0, 32, 32);
        tquad(210 + 20 * (float)l, 24, 6, 6, 0, 0, 1, 1);
        gx_begin(GX_QUADS, 0, 4);                     /* rombo: la derivada diagonal */
        v(320 + 40 * (float)l, 24, 0, 0xFFFFFFFFu, 0, 0);
        v(340 + 40 * (float)l, 34, 0, 0xFFFFFFFFu, 1, 0);
        v(320 + 40 * (float)l, 44, 0, 0xFFFFFFFFu, 1, 1);
        v(300 + 40 * (float)l, 34, 0, 0xFFFFFFFFu, 0, 1);
    }
    snap(0, 0, 640, 250);

    /* Un piso en perspectiva que se aleja: el LOD cambia de un bloque a otro */
    gx_projection(persp, 0);
    gx_xf_floats(XF_VIEWPORT, 6, viewport_persp);
    for (uint32_t mip = 0; mip < 3; mip++) {
        tex_unit(0, MODE0(WRAP_REPEAT, WRAP_MIRROR, 1, mip, 1, 0, 0), MODE1(0, 0xA0), 32, 32, TF_RGB5A3, t,
                 0, 0);
        use_map(0, 32, 32);
        const float x = -4.5f + 3.0f * (float)mip;
        gx_begin(GX_QUADS, 0, 4);
        v(x, -1, -2, 0xFFFFFFFFu, 0, 0);
        v(x + 2.5f, -1, -2, 0xFFFFFFFFu, 2, 0);
        v(x + 2.5f, -1, -30, 0xFFFFFFFFu, 2, 12);
        v(x, -1, -30, 0xFFFFFFFFu, 0, 12);
    }
    gx_projection(ortho_px, 1);
    gx_xf_floats(XF_VIEWPORT, 6, viewport_px);
    snap(0, 250, 640, 278);
    sync_gpu("4. mipmaps");
}

/* ---- 5. TMEM ------------------------------------------------------------------------ */

static void tmem(void)
{
    clear_efb(0x000000FFu);
    /* I8 de 16 × 16 (8 líneas) a la línea 8192 */
    const uint8_t *i8 = random_bytes(256);
    gx_bp(BP_PRELOAD_ADDR, gx_phys(i8) >> 5);
    gx_bp(BP_PRELOAD_EVEN, 8192);
    gx_bp(BP_PRELOAD_MODE, 8);
    tex_unit(0, MODE0(WRAP_REPEAT, WRAP_REPEAT, 0, 0, 0, 0, 0), 0, 16, 16, TF_I8, NULL, 0, 0);
    tex_unit_tmem(0, 8192, 0);
    use_map(0, 16, 16);
    tquad(4, 4, 32, 32, 0, 0, 1, 1);

    /* RGBA8 de 16 × 16: 16 pares de bloques, alfa y rojo a la línea 9000 y
     * verde y azul a la 11000 */
    const uint8_t *rgba = random_bytes(1024);
    gx_bp(BP_PRELOAD_ADDR, gx_phys(rgba) >> 5);
    gx_bp(BP_PRELOAD_EVEN, 9000);
    gx_bp(BP_PRELOAD_ODD, 11000);
    gx_bp(BP_PRELOAD_MODE, 16 | 3u << 15);
    tex_unit(0, MODE0(WRAP_CLAMP, WRAP_CLAMP, 1, 0, 0, 0, 0), 0, 16, 16, TF_RGBA8, NULL, 0, 0);
    tex_unit_tmem(0, 9000, 11000);
    use_map(0, 16, 16);
    tquad(40, 4, 32, 32, 0, 0, 1, 1);
    /* La misma con mipmaps: al bajar de nivel solo avanza el banco par */
    tex_unit(0, MODE0(WRAP_CLAMP, WRAP_CLAMP, 1, MIP_POINT, 1, 0, 0), MODE1(0, 0x30), 16, 16, TF_RGBA8,
             NULL, 0, 0);
    tex_unit_tmem(0, 9000, 11000);
    tquad(76, 4, 8, 8, 0, 0, 1, 1);
    tquad(88, 4, 4, 4, 0, 0, 1, 1);

    /* C4 precargado con la paleta de la TMEM */
    const uint8_t *c4 = random_bytes(128);
    gx_bp(BP_PRELOAD_ADDR, gx_phys(c4) >> 5);
    gx_bp(BP_PRELOAD_EVEN, 12000);
    gx_bp(BP_PRELOAD_MODE, 4);
    gx_bp(BP_TEXINVALIDATE, 0);                 /* no cambia nada en el video por software */
    tex_unit(0, 0, 0, 16, 16, TF_C4, NULL, 512, 1);
    tex_unit_tmem(0, 12000, 0);
    use_map(0, 16, 16);
    tquad(100, 4, 32, 32, 0, 0, 1, 1);
    snap(0, 0, 640, 40);
    sync_gpu("5. TMEM");
}

/* ---- 6. unidades -------------------------------------------------------------------- */

static void units(void)
{
    clear_efb(0x000000FFu);
    for (uint32_t map = 1; map < 8; map++) {
        const uint8_t *t = random_bytes(tex_bytes(TF_IA8, 8, 8));
        tex_unit(map, MODE0(WRAP_CLAMP, WRAP_CLAMP, map & 1u, 0, 0, 0, 0), 0, 8, 8, TF_IA8, t, 0, 0);
    }
    /* Pisar la unidad 0 para ver que no se usa */
    tex_unit(0, 0, 0, 8, 8, TF_I8, random_bytes(64), 0, 0);
    for (uint32_t map = 1; map < 8; map++) {
        use_map(map, 8, 8);
        tquad(4 + 36 * (float)(map - 1), 4, 32, 32, 0, 0, 1, 1);
    }
    snap(0, 0, 260, 40);
    sync_gpu("6. unidades");
}

/* ---- 7. textura por color y mezcla con su alfa -------------------------------------- */

static void modulate(void)
{
    clear_efb(0x3060A0FFu);
    const uint8_t *t = random_bytes(tex_bytes(TF_RGB5A3, 16, 16));
    tex_unit(0, MODE0(WRAP_REPEAT, WRAP_REPEAT, 1, 0, 1, 0, 0), 0, 16, 16, TF_RGB5A3, t, 0, 0);
    use_map(0, 16, 16);
    gx_bp(BP_COLOR_C0, CC(15, 8, 10, 15));      /* tex × ras */
    gx_bp(BP_ALPHA_C0, AC(7, 4, 5, 7));
    gx_bp(BP_BLEND, 1u | 5u << 5 | 4u << 8 | 1u << 3 | 1u << 4);   /* alfa de la fuente */
    gx_begin(GX_QUADS, 0, 4);
    v(10, 10, 0, 0xFF0000FFu, 0, 0);
    v(200, 10, 0, 0x00FF00C0u, 3, 0);
    v(200, 150, 0, 0x0000FF80u, 3, 2);
    v(10, 150, 0, 0xFFFFFF40u, 0, 2);
    gx_bp(BP_BLEND, 1u << 3 | 1u << 4);
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 8));
    gx_bp(BP_ALPHA_C0, AC(7, 7, 7, 4));
    snap(0, 0, 220, 160);
    snap(0, 0, 640, 528);
    sync_gpu("7. textura por color y mezcla");
}

int main(void)
{
    OSReport("gx_tex: texturas\n");
    setup();
    formats();
    wraps();
    filters();
    mipmaps();
    tmem();
    units();
    modulate();
    OSReport("gx_tex: %u tokens, %u bytes de texturas; %s\n", (unsigned)token, (unsigned)pool_used,
             fails ? "FALLO" : "OK");
    exit(fails ? 1 : 0);
}
