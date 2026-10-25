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
 * DOL de prueba "gx_copy" (Fase 2, paso C6): copias del EFB a la RAM con la
 * mini-librería propia (gxlib, gxvtx y video, sin libogc).
 *
 *   1-3. a textura desde RGB8, RGBA6 y RGB565: los 13 formatos, con y sin
 *        intensidad, enteros y a media escala;
 *   4. desde la Z: los 8 formatos que tiene Dolphin, enteros y a media escala;
 *   5. rectángulos raros: tamaños que no llenan el bloque, la x y la y
 *      impares, las últimas filas (el relleno lee la Z), uno que se pasa del
 *      EFB, pasos menores y mayores que la fila, borrar al copiar e
 *      intensidad sin conversión automática;
 *   6. una copia usada como textura: RGB565, I8 y RGBA8 con alfa;
 *   7. al XFB: filtros (el normal, el antiparpadeo y uno de brillo), los
 *      recortes de arriba y de abajo, las 4 gammas, escalas verticales, un
 *      paso mayor que la fila, el rectángulo corrido a la derecha y desde
 *      RGBA6;
 *   8. la imagen en pantalla: la escena copiada al XFB que muestra la VI.
 *
 * Lo que en Dolphin es un PanicAlert o lee fuera de sus búferes no se usa:
 * formatos 0xD-0xF, desde la Z RA4, RA8, RGB565, RGB5A3 y A8, el relleno de
 * una copia de Z que pasa de la última fila, anchos impares del XFB con la x
 * distinta de 0, la escala de 2 o más con 528 filas, pasos de más de 10 240
 * bytes y más de 1024 filas.
 *
 * Se compara contra el renderizador por software de Dolphin: cada triángulo y
 * cada copia con el CRC del EFB y de lo que quedó en la RAM
 * (tests/dol/dolphin/gx_copy.cop.txt).
 *
 * Solo para el DOL: no compila en la PC.
 */
#include <stddef.h>

#include "gxlib.h"
#include "rt.h"
#include "video.h"

#define FIFO_SIZE (64u * 1024u)
#define MAX_WAIT  2000000u
#define POOL      0x00400000u                   /* copias a textura: de 4 a 12 MB */
#define POOL_END  0x00C00000u
#define SCRATCH   0x00C00000u                   /* copias al XFB de prueba */

static uint8_t fifo[FIFO_SIZE] __attribute__((aligned(32)));
static uint32_t pool = POOL;
static uint32_t token;
static int fails;

enum {
    BP_TEVORDER = 0x28, BP_ZMODE = 0x40, BP_BLEND = 0x41, BP_ZCONTROL = 0x43, BP_COPY_XY = 0x49,
    BP_COPY_WH = 0x4A, BP_COPY_DST = 0x4B, BP_COPY_STRIDE = 0x4D, BP_COPY_YSCALE = 0x4E, BP_CLEAR_AR = 0x4F,
    BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51, BP_COPY = 0x52, BP_COPY_FILTER0 = 0x53, BP_COPY_FILTER1 = 0x54,
    BP_SCISSOR_OFFSET = 0x59, BP_COLOR_C0 = 0xC0, BP_ALPHA_C0 = 0xC1, BP_ALPHATEST = 0xF3, BP_KSEL = 0xF6,
};
enum { PF_RGB8 = 0, PF_RGBA6 = 1, PF_RGB565 = 2, PF_Z24 = 3 };
enum { R4 = 0x0, R8_1 = 0x1, RA4 = 0x2, RA8 = 0x3, RGB565 = 0x4, RGB5A3 = 0x5, RGBA8 = 0x6, A8 = 0x7, R8 = 0x8,
       G8 = 0x9, B8 = 0xA, RG8 = 0xB, GB8 = 0xC };
enum { TF_I8 = 1, TF_RGB565 = 4, TF_RGBA8 = 6 };

#define ZMODE(func, update) (1u | (uint32_t)(func) << 1 | (uint32_t)(update) << 4)
#define B_COLOR_ALPHA (1u << 3 | 1u << 4)
#define CC(a, b, c, d) ((uint32_t)(a) << 12 | (uint32_t)(b) << 8 | (uint32_t)(c) << 4 | (uint32_t)(d) | 1u << 19)
#define AC(a, b, c, d) ((uint32_t)(a) << 13 | (uint32_t)(b) << 10 | (uint32_t)(c) << 7 | (uint32_t)(d) << 4 | 1u << 19)
/* Disparo de BP 0x52 */
#define T_CLAMP_TOP    1u
#define T_CLAMP_BOTTOM 2u
#define T_GAMMA(g)     ((uint32_t)(g) << 7)
#define T_HALF         (1u << 9)
#define T_INVERT       (1u << 10)
#define T_CLEAR        (1u << 11)
#define T_XFB          (1u << 14)
#define T_INTENSITY    (1u << 15)
#define T_AUTO         (1u << 16)

static const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
/* Ortográfica en píxeles exactos (como gx_raster): x, y del EFB y z de 24 bits */
static const float ortho_px[6] = { 1.0f / 512.0f, -1.0f, -1.0f / 512.0f, 1.0f, 1.0f / 16777216.0f, -1.0f };
static const float viewport_px[6] = { 512.0f, -512.0f, 16777216.0f, 854.0f, 854.0f, 16777216.0f };

/* ---- utilidades -------------------------------------------------------------------- */

static void wait_gpu(const char *what)
{
    token++;
    gx_bp(BP_TOKEN, token);
    gx_flush();
    if (!gx_wait_idle(MAX_WAIT)) {
        OSReport("gx_copy: %s: la GPU no vacio el FIFO\n", what ? what : "copia");
        fails++;
        return;
    }
    uint32_t n = 0;
    while (PE_REG(PE_TOKEN) != (uint16_t)token && n < MAX_WAIT)
        n++;
    if (n == MAX_WAIT) {
        OSReport("gx_copy: %s: el token %u no llego\n", what ? what : "copia", (unsigned)token);
        fails++;
        return;
    }
    if (what)
        OSReport("gx_copy: %s\n", what);
}

static uint32_t seed = 0x3C6EF372u;

static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

static uint32_t rnd32(void)
{
    return rnd() << 8 ^ rnd();
}

/* El formato real (tp / 2 + (tp & 1) × 8) al campo de BP 0x52 */
static uint32_t tp(uint32_t fmt)
{
    return (fmt < 8 ? fmt * 2 : (fmt - 8) * 2 + 1) << 3;
}

/* Bloques del formato base de la copia (TexDecoder_GetEFBCopyBaseFormat) */
static void block(uint32_t fmt, uint32_t *bw, uint32_t *bh, uint32_t *bpb)
{
    *bpb = 32;
    if (fmt == R4) {
        *bw = 8;
        *bh = 8;
    } else if (fmt == RA8 || fmt == RGB565 || fmt == RGB5A3 || fmt == RGBA8 || fmt == RG8 || fmt == GB8) {
        *bw = 4;
        *bh = 4;
        if (fmt == RGBA8)
            *bpb = 64;
    } else {
        *bw = 8;
        *bh = 4;
    }
}

/* Bytes por fila de bloques de una copia de w × h a textura */
static uint32_t row_bytes(uint32_t fmt, uint32_t w, uint32_t half)
{
    uint32_t bw, bh, bpb;
    block(fmt, &bw, &bh, &bpb);
    w >>= half;
    return (w + bw - 1) / bw * bpb;
}

static uint32_t rows_of(uint32_t fmt, uint32_t h, uint32_t half)
{
    uint32_t bw, bh, bpb;
    block(fmt, &bw, &bh, &bpb);
    h >>= half;
    return (h + bh - 1) / bh;
}

static void copy_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    gx_bp(BP_COPY_XY, x | y << 10);
    gx_bp(BP_COPY_WH, (w - 1) | (h - 1) << 10);
}

/* Copia a la RAM en dest con ese paso */
static void copy_to(uint32_t dest, uint32_t stride, uint32_t trig)
{
    gx_bp(BP_COPY_DST, dest >> 5);
    gx_bp(BP_COPY_STRIDE, stride >> 5);
    gx_bp(BP_COPY, trig);
}

/* Lugar en la RAM para una copia */
static uint32_t alloc(uint32_t n)
{
    const uint32_t p = pool;
    pool += (n + 31u) & ~31u;
    if (pool > POOL_END) {
        OSReport("gx_copy: sin lugar para las copias\n");
        fails++;
        exit(1);
    }
    return p;
}

/* Una copia a textura de x, y, w, h con el paso justo */
static void tex_copy(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags)
{
    const uint32_t half = (flags & T_HALF) ? 1 : 0;
    const uint32_t stride = row_bytes(fmt, w, half);
    copy_rect(x, y, w, h);
    copy_to(alloc(stride * rows_of(fmt, h, half)), stride, tp(fmt) | flags);
}

/* Copia sin destino: el CRC del rectángulo del EFB */
static void snap(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    copy_rect(x, y, w, h);
    copy_to(0, 0, 0);
}

static void clear_efb(uint32_t rgba, uint32_t z)
{
    gx_bp(BP_ZMODE, ZMODE(7, 1));
    gx_bp(BP_CLEAR_AR, (rgba & 0xFFu) << 8 | rgba >> 24);
    gx_bp(BP_CLEAR_GB, (rgba >> 8) & 0xFFFFu);
    gx_bp(BP_CLEAR_Z, z);
    copy_rect(0, 0, 640, 528);
    copy_to(0, 0, T_CLEAR);
    gx_bp(BP_ZMODE, 0);
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

/* Lo que se copia: un degradé de esquina a esquina y triángulos al azar, con
 * colores, alfas y Z distintos */
static void scene(void)
{
    gx_bp(BP_ZMODE, ZMODE(7, 1));
    gx_begin(GX_QUADS, 0, 4);
    v(0, 0, 0x100000, 0xFF2040C0u, 0, 0);
    v(640, 0, 0x500000, 0x20FF60FFu, 0, 0);
    v(640, 528, 0xF00000, 0x4020FF20u, 0, 0);
    v(0, 528, 0xA00000, 0xFFFF2080u, 0, 0);
    for (int i = 0; i < 24; i++) {
        gx_begin(GX_TRIANGLES, 0, 3);
        for (int k = 0; k < 3; k++)
            v((float)(rnd() % 640), (float)(rnd() % 528), (float)rnd(), rnd32(), 0, 0);
    }
    gx_bp(BP_ZMODE, 0);
}

/* El formato de píxel y una escena nueva */
static void efb_format(uint32_t pf)
{
    gx_bp(BP_ZCONTROL, pf);
    clear_efb(0x000000FFu, 0xFFFFFFu);
    scene();
}

/* ---- 0. estado inicial -------------------------------------------------------------- */

static void setup(void)
{
    gx_init_fifo(fifo, FIFO_SIZE, FIFO_SIZE - 16384, FIFO_SIZE / 2, CP_CR_READ | CP_CR_LINK);
    gx_bp(BP_GENMODE, 1 | 1u << 4);
    gx_xf1(XF_NUMTEXGEN, 1);
    gx_xf1(XF_NUMCHAN, 1);
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
    gx_bp(BP_TEVORDER, 0);                      /* COLOR0, sin textura */
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 10));     /* PREV = ras */
    gx_bp(BP_ALPHA_C0, AC(7, 7, 7, 5));
    gx_bp(BP_KSEL, 1u << 2);                    /* swap 0: r g b a */
    gx_bp(BP_KSEL + 1, 2u | 3u << 2);
    gx_bp(BP_ALPHATEST, 7u << 16 | 7u << 19);
    gx_bp(BP_ZMODE, 0);
    gx_bp(BP_BLEND, B_COLOR_ALPHA);
    gx_bp(BP_COPY_FILTER0, 0 | 0u << 6 | 21u << 12 | 22u << 18);   /* filtro normal del SDK */
    gx_bp(BP_COPY_FILTER1, 21 | 0u << 6 | 0u << 12);
    gx_bp(BP_COPY_YSCALE, 256);
    wait_gpu("estado inicial");
}

/* ---- 1-4. a textura ----------------------------------------------------------------- */

static void all_formats(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    for (uint32_t half = 0; half < 2; half++)
        for (uint32_t fmt = 0; fmt <= GB8; fmt++) {
            const uint32_t flags = half ? T_HALF : 0;
            tex_copy(x, y, w, h, fmt, flags);
            if (fmt <= RA8 || fmt == R8)        /* I4, I8, IA4, IA8 */
                tex_copy(x, y, w, h, fmt, flags | T_INTENSITY | T_AUTO);
        }
}

static void from_color(uint32_t pf, const char *what)
{
    efb_format(pf);
    all_formats(36, 42, 72, 38);
    wait_gpu(what);
}

static void from_depth(void)
{
    static const uint32_t zf[8] = { R4, R8_1, RGBA8, R8, G8, B8, RG8, GB8 };
    efb_format(PF_Z24);
    for (uint32_t half = 0; half < 2; half++)
        for (uint32_t i = 0; i < 8; i++)
            tex_copy(100, 200, 64, 40, zf[i], half ? T_HALF : 0);
    tex_copy(100, 200, 64, 40, R8, T_INTENSITY | T_AUTO);   /* la Z no la usa */
    wait_gpu("4. desde la Z");
}

/* ---- 5. rectángulos raros ----------------------------------------------------------- */

static void odd_rects(void)
{
    efb_format(PF_RGB8);
    tex_copy(3, 7, 6, 5, R8, 0);                /* menos que un bloque, en x e y impares */
    tex_copy(5, 9, 14, 11, RGB565, T_HALF);
    tex_copy(100, 523, 40, 5, R4, 0);           /* el relleno lee las primeras filas de Z */
    tex_copy(200, 520, 24, 8, RGBA8, T_HALF);
    /* Se pasa por la derecha: se recorta a 40, pero los bloques salen de los 64;
     * los 6 de más quedan en el búfer intermedio y los lee una copia al XFB que
     * pide una fila más de las que escribe (escala 0,5 con 3 de alto) */
    copy_rect(600, 100, 64, 16);
    copy_to(alloc(1024 * 4), 1024, tp(RGBA8));
    tex_copy(8, 8, 8, 4, R8, 0);                /* una chica en el medio: no los pisa */
    copy_rect(0, 0, 320, 3);
    gx_bp(BP_COPY_YSCALE, 128);
    copy_to(SCRATCH, 640, T_XFB);
    gx_bp(BP_COPY_YSCALE, 256);
    copy_rect(600, 500, 32, 40);               /* y por abajo */
    copy_to(alloc(256 * 8), 256, tp(R8));
    /* Paso menor que la fila (se mezcla) y mayor (quedan huecos) */
    copy_rect(64, 64, 64, 16);
    copy_to(alloc(512 * 4), 512, tp(RGBA8));
    copy_to(alloc(1120 * 4), 1120, tp(RGBA8));
    copy_to(alloc(32 * 4), 32, tp(RGB565));
    /* Intensidad sin conversión automática: como R8 */
    tex_copy(64, 64, 32, 16, R8, T_INTENSITY);
    /* RGBA6 con alfa 55 de 6 bits: a media escala da 223, justo debajo del
     * corte de RGB5A3 (224) */
    gx_bp(BP_ZCONTROL, PF_RGBA6);
    gx_bp(BP_CLEAR_AR, 220u << 8 | 0x90u);
    gx_bp(BP_CLEAR_GB, 0x50D0u);
    copy_rect(400, 400, 16, 8);
    copy_to(0, 0, T_CLEAR);
    tex_copy(400, 400, 16, 8, RGB5A3, T_HALF);
    gx_bp(BP_CLEAR_AR, 224u << 8 | 0x90u);
    copy_rect(400, 400, 16, 8);
    copy_to(0, 0, T_CLEAR);
    tex_copy(400, 400, 16, 8, RGB5A3, T_HALF);
    gx_bp(BP_ZCONTROL, PF_RGB8);
    /* Borrar al copiar, y copiar lo borrado */
    gx_bp(BP_CLEAR_AR, 0x40u << 8 | 0x80u);
    gx_bp(BP_CLEAR_GB, 0x20C0u);
    tex_copy(300, 300, 32, 32, RGBA8, T_CLEAR);
    tex_copy(290, 290, 48, 48, RGBA8, 0);
    wait_gpu("5. rectangulos raros");
}

/* ---- 6. copias como texturas -------------------------------------------------------- */

static void tex_unit0(uint32_t addr, uint32_t w, uint32_t h, uint32_t fmt)
{
    gx_bp(0x80, 0);                             /* recortar, más cercano */
    gx_bp(0x84, 0);
    gx_bp(0x88, (w - 1) | (h - 1) << 10 | fmt << 20);
    gx_bp(0x8C, 0);
    gx_bp(0x94, addr >> 5);
    gx_bp(BP_SU_SSIZE, w - 1);
    gx_bp(BP_SU_TSIZE, h - 1);
}

static void textured(float x, float y, float w, float h, uint32_t c)
{
    gx_begin(GX_QUADS, 0, 4);
    v(x, y, 0, c, 0, 0);
    v(x + w, y, 0, c, 1, 0);
    v(x + w, y + h, 0, c, 1, 1);
    v(x, y + h, 0, c, 0, 1);
}

static void render_to_texture(void)
{
    efb_format(PF_RGBA6);
    const uint32_t a565 = alloc(64 * 64 * 2), ai8 = alloc(64 * 64), argba = alloc(64 * 64 * 4);
    copy_rect(40, 40, 64, 64);
    copy_to(a565, 16 * 32, tp(RGB565));            /* 16 bloques de 32 por fila */
    copy_to(ai8, 8 * 32, tp(R8) | T_INTENSITY | T_AUTO);
    copy_to(argba, 16 * 64, tp(RGBA8));
    gx_bp(BP_TEVORDER, 0 | 1u << 6);            /* textura 0, coordenada 0, COLOR0 */
    gx_bp(BP_COLOR_C0, CC(15, 8, 10, 15));      /* tex × ras */
    gx_bp(BP_ALPHA_C0, AC(7, 4, 5, 7));
    tex_unit0(a565, 64, 64, TF_RGB565);
    textured(300, 40, 128, 128, 0xFFFFFFFFu);
    tex_unit0(ai8, 64, 64, TF_I8);
    textured(440, 40, 128, 64, 0xFF8040FFu);
    /* RGBA8 mezclada con su alfa */
    gx_bp(BP_BLEND, 1u | 4u << 5 | 5u << 8 | B_COLOR_ALPHA);
    tex_unit0(argba, 64, 64, TF_RGBA8);
    textured(300, 200, 192, 192, 0xFFFFFFFFu);
    gx_bp(BP_BLEND, B_COLOR_ALPHA);
    gx_bp(BP_TEVORDER, 0);
    gx_bp(BP_COLOR_C0, CC(15, 15, 15, 10));
    gx_bp(BP_ALPHA_C0, AC(7, 7, 7, 5));
    snap(0, 0, 640, 528);
    wait_gpu("6. copias como texturas");
}

/* ---- 7. al XFB ---------------------------------------------------------------------- */

static void filter(const uint8_t k[7])
{
    gx_bp(BP_COPY_FILTER0, k[0] | (uint32_t)k[1] << 6 | (uint32_t)k[2] << 12 | (uint32_t)k[3] << 18);
    gx_bp(BP_COPY_FILTER1, k[4] | (uint32_t)k[5] << 6 | (uint32_t)k[6] << 12);
}

/* Al XFB de prueba: ancho w (par), alto h, con su escala */
static void xfb_copy(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t yscale, uint32_t stride,
                     uint32_t trig)
{
    copy_rect(x, y, w, h);
    gx_bp(BP_COPY_YSCALE, yscale);
    copy_to(SCRATCH, stride, T_XFB | trig);
}

static void to_xfb(void)
{
    static const uint8_t normal[7] = { 0, 0, 21, 22, 21, 0, 0 };
    static const uint8_t deflicker[7] = { 8, 8, 10, 12, 10, 8, 8 };
    static const uint8_t bright[7] = { 16, 16, 16, 16, 16, 16, 16 };
    efb_format(PF_RGB8);
    filter(deflicker);
    for (uint32_t clamp = 0; clamp < 4; clamp++) {
        xfb_copy(0, 0, 320, 120, 256, 640, clamp);
        xfb_copy(0, 200, 320, 120, 256, 640, clamp);
        xfb_copy(0, 408, 320, 120, 256, 640, clamp);
    }
    filter(normal);
    xfb_copy(0, 200, 320, 120, 256, 640, 0);
    filter(bright);
    xfb_copy(64, 64, 256, 64, 256, 512, T_CLAMP_TOP | T_CLAMP_BOTTOM);
    filter(normal);
    for (uint32_t g = 0; g < 4; g++)
        xfb_copy(0, 100, 640, 40, 256, 1280, T_GAMMA(g) | T_CLAMP_TOP | T_CLAMP_BOTTOM);
    /* Escalas: 1,28 (256 / 200), 2 (con filas de una copia anterior) y 0,5
     * con un alto impar (la última fila es del búfer intermedio) */
    xfb_copy(0, 0, 640, 100, 200, 1280, T_INVERT);
    xfb_copy(0, 0, 640, 100, 512, 1280, 0);
    xfb_copy(0, 0, 640, 99, 128, 1280, 0);
    /* Paso mayor que la fila: 600 píxeles (1216 bytes) cada 1280 */
    xfb_copy(0, 50, 600, 60, 256, 1280, 0);
    /* Corrido a la derecha: Dolphin escribe en x y lee desde 0 */
    xfb_copy(32, 50, 256, 60, 256, 512, 0);
    xfb_copy(0, 50, 640, 2, 256, 1280, 0);
    /* Desde RGBA6 */
    efb_format(PF_RGBA6);
    xfb_copy(0, 0, 640, 120, 256, 1280, T_CLAMP_TOP | T_CLAMP_BOTTOM);
    gx_bp(BP_COPY_YSCALE, 256);
    wait_gpu("7. al XFB");
}

/* ---- 8. en pantalla ----------------------------------------------------------------- */

static void on_screen(void)
{
    efb_format(PF_RGB8);
    copy_rect(0, 0, 640, 480);
    gx_bp(BP_COPY_YSCALE, 256);
    copy_to(XFB_PHYS, 1280, T_XFB | T_CLAMP_TOP | T_CLAMP_BOTTOM);
    wait_gpu("8. en pantalla");
    video_init_480i();
    video_wait_frame();
    video_wait_frame();
}

int main(void)
{
    OSReport("gx_copy: copias del EFB\n");
    setup();
    from_color(PF_RGB8, "1. a textura desde RGB8");
    from_color(PF_RGBA6, "2. a textura desde RGBA6");
    from_color(PF_RGB565, "3. a textura desde RGB565");
    from_depth();
    odd_rects();
    render_to_texture();
    to_xfb();
    on_screen();
    OSReport("gx_copy: %u tokens, %u bytes de copias; %s\n", (unsigned)token, (unsigned)(pool - POOL),
             fails ? "FALLO" : "OK");
    exit(fails ? 1 : 0);
}
