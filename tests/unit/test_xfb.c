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

/* La imagen: YUV a RGB, el cuadro con los dos campos, el PNG y el CRC-32. */
#include <stdlib.h>
#include <string.h>

#include "core/crc32.h"
#include "core/frames.h"
#include "core/log.h"
#include "core/png.h"
#include "core/system.h"
#include "core/xfb.h"
#include "dol_build.h"
#include "test.h"

static int near(int a, int b) { return a - b <= 2 && b - a <= 2; }

static void test_crc32(void)
{
    CHECK_EQ_U64(gc_crc32(0, "123456789", 9), 0xCBF43926u);
    CHECK_EQ_U64(gc_crc32(gc_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
    CHECK_EQ_U64(gc_crc32(0, "", 0), 0);
}

/* Las barras al 75 % de tests/dol/video.c y los extremos del rango de estudio */
static void test_yuv_to_rgb(void)
{
    static const struct { uint32_t yuyv; int r, g, b; } bars[] = {
        { 0xB480B480u, 191, 191, 191 }, { 0xA12CA18Du, 191, 191, 0 }, { 0x839C832Cu, 0, 191, 191 },
        { 0x7049703Au, 0, 191, 0 },     { 0x54B754C6u, 191, 0, 191 }, { 0x416441D4u, 191, 0, 0 },
        { 0x23D42373u, 0, 0, 191 },     { 0x10801080u, 0, 0, 0 },
    };
    uint8_t rgb[3];
    for (size_t k = 0; k < sizeof bars / sizeof bars[0]; k++) {
        const uint32_t w = bars[k].yuyv;
        gc_yuv_to_rgb((uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)w, rgb);
        CHECK(near(rgb[0], bars[k].r) && near(rgb[1], bars[k].g) && near(rgb[2], bars[k].b));
    }
    gc_yuv_to_rgb(235, 128, 128, rgb);
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);
    gc_yuv_to_rgb(16, 128, 128, rgb);
    CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);
    gc_yuv_to_rgb(16, 0, 255, rgb);                    /* fuera de rango: recorta en 0... */
    CHECK(rgb[0] == 203 && rgb[1] == 0 && rgb[2] == 0);
    gc_yuv_to_rgb(255, 255, 128, rgb);                 /* ...y en 255 */
    CHECK(rgb[0] == 255 && rgb[2] == 255);
    gc_yuv_to_rgb(42, 161, 110, rgb);                  /* el fondo de los DOLs: RGB (0, 32, 96) */
    CHECK(rgb[0] == 2 && rgb[1] == 32 && rgb[2] == 97);

    /* Una línea: cada par comparte U y V */
    static const uint8_t line[8] = { 235, 128, 16, 128, 82, 90, 82, 240 };
    uint8_t out[12];
    gc_xfb_line_to_rgb(line, 4, out);
    CHECK(out[0] == 255 && out[3] == 0 && out[4] == 0);
    gc_yuv_to_rgb(82, 90, 240, rgb);
    CHECK(memcmp(out + 6, rgb, 3) == 0 && memcmp(out + 9, rgb, 3) == 0);
}

/* ---- el cuadro con los dos campos -------------------------------------------------- */

static gc_system_t sys;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

#define RED  0x416441D4u
#define BLUE 0x23D42373u

static void fill_field(uint32_t addr, uint32_t width, uint32_t height, uint32_t stride, uint32_t c)
{
    for (uint32_t k = 0; k < height; k++)
        for (uint32_t x = 0; x < width; x += 2)
            gc_write32(&sys.mem, 0x80000000u | (addr + k * stride + x * 2), c);
}

static gc_vi_field_t field(uint32_t xfb, uint32_t width, uint32_t height, uint32_t stride, int even)
{
    gc_vi_field_t f = { xfb, width, stride, height, even };
    return f;
}

/* Fila y del cuadro w de ancho: ¿todos sus píxeles son el color c (en YUV)? */
static int row_is(const uint8_t *rgb, uint32_t w, uint32_t y, uint32_t c)
{
    uint8_t want[3];
    gc_yuv_to_rgb((uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)c, want);
    for (uint32_t x = 0; x < w; x++)
        if (memcmp(rgb + ((size_t)y * w + x) * 3, want, 3) != 0)
            return 0;
    return 1;
}

static void test_weave(void)
{
    static uint8_t rgb[16 * 8 * 3];
    uint32_t w, h;
    gc_log_set_sink(quiet);
    CHECK(gc_system_init(&sys) == 0);

    CHECK_EQ_U64(gc_xfb_frame_size(&sys.vi, &w, &h), 0);       /* sin campos */
    CHECK(gc_xfb_frame(&sys.mem, &sys.vi, rgb, sizeof rgb) == -1);

    fill_field(0x100000, 16, 4, 64, RED);                       /* impar: rojo */
    fill_field(0x100020, 16, 4, 64, BLUE);                      /* par: azul */
    sys.vi.field[0] = field(0x100000, 16, 4, 64, 0);
    sys.vi.field[1] = field(0x100020, 16, 4, 64, 1);
    CHECK_EQ_U64(gc_xfb_frame_size(&sys.vi, &w, &h), sizeof rgb);
    CHECK(w == 16 && h == 8);
    CHECK(gc_xfb_frame(&sys.mem, &sys.vi, rgb, sizeof rgb - 1) == -1);   /* no entra */
    CHECK(gc_xfb_frame(&sys.mem, &sys.vi, rgb, sizeof rgb) == 0);
    for (uint32_t y = 0; y < 8; y++)                            /* PRB 502 / 503: impar arriba */
        CHECK(row_is(rgb, 16, y, (y & 1) ? BLUE : RED));

    gc_write16(&sys.mem, 0xCC002000u + VI_VTE + 2, 501);       /* PRB par menor: par arriba */
    CHECK(!gc_vi_odd_on_top(&sys.vi));
    CHECK(gc_xfb_frame(&sys.mem, &sys.vi, rgb, sizeof rgb) == 0);
    CHECK(row_is(rgb, 16, 0, BLUE) && row_is(rgb, 16, 1, RED));
    gc_write16(&sys.mem, 0xCC002000u + VI_VTE + 2, 503);

    sys.vi.field[1].xfb = 0;                                    /* un solo campo: se repite */
    CHECK(gc_xfb_frame(&sys.mem, &sys.vi, rgb, sizeof rgb) == 0);
    for (uint32_t y = 0; y < 8; y++)
        CHECK(row_is(rgb, 16, y, RED));

    sys.vi.field[1] = field(0x100020, 16, 3, 64, 1);           /* al par le falta una línea */
    CHECK(gc_xfb_frame(&sys.mem, &sys.vi, rgb, sizeof rgb) == 0);
    CHECK(row_is(rgb, 16, 5, BLUE) && row_is(rgb, 16, 6, RED));
    CHECK(row_is(rgb, 16, 7, 0x10801080u));                     /* negro */

    sys.vi.field[1] = field(0x017FFFF0, 16, 4, 64, 1);         /* fuera de la RAM: negro */
    CHECK(gc_xfb_frame(&sys.mem, &sys.vi, rgb, sizeof rgb) == 0);
    CHECK(row_is(rgb, 16, 1, 0x10801080u) && row_is(rgb, 16, 0, RED));
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

/* ---- PNG ----------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Lee el PNG que escribe gc_png_encode: bloques con su CRC, zlib con bloques
 * "stored" y su Adler-32, filtro 0 en cada fila. 0 si los píxeles son rgb. */
static int png_check(const uint8_t *png, size_t len, const uint8_t *rgb, uint32_t w, uint32_t h)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (len < 8 || memcmp(png, sig, 8) != 0)
        return -1;
    const size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(raw_len ? raw_len : 1);
    size_t got = 0, pos = 8;
    int seen_end = 0, rc = 0;
    while (!rc && pos + 12 <= len && !seen_end) {
        const uint32_t n = be32(png + pos);
        const uint8_t *type = png + pos + 4, *data = png + pos + 8;
        if (pos + 12 + n > len || gc_crc32(0, type, 4 + (size_t)n) != be32(data + n)) {
            rc = -1;
            break;
        }
        if (memcmp(type, "IHDR", 4) == 0) {
            if (n != 13 || be32(data) != w || be32(data + 4) != h || data[8] != 8 || data[9] != 2)
                rc = -1;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (n < 6 || data[0] != 0x78 || (data[0] * 256 + data[1]) % 31 != 0) {
                rc = -1;
                break;
            }
            size_t p = 2;
            int last = 0;
            while (!last && p + 5 <= n) {
                last = data[p] & 1;
                const uint32_t blen = data[p + 1] | (uint32_t)data[p + 2] << 8;
                const uint32_t nlen = data[p + 3] | (uint32_t)data[p + 4] << 8;
                if ((data[p] & 6) != 0 || (blen ^ nlen) != 0xFFFF || got + blen > raw_len ||
                    p + 5 + blen > n) {
                    rc = -1;
                    break;
                }
                memcpy(raw + got, data + p + 5, blen);
                got += blen;
                p += 5 + blen;
            }
            uint32_t a = 1, b = 0;
            for (size_t k = 0; k < got; k++) {
                a = (a + raw[k]) % 65521u;
                b = (b + a) % 65521u;
            }
            if (!last || p + 4 != n || be32(data + p) != (b << 16 | a))
                rc = -1;
        } else if (memcmp(type, "IEND", 4) == 0) {
            seen_end = 1;
        }
        pos += 12 + n;
    }
    if (!rc && (!seen_end || pos != len || got != raw_len))
        rc = -1;
    for (uint32_t y = 0; !rc && y < h; y++)
        if (raw[(size_t)y * (1 + w * 3)] != 0 ||
            memcmp(raw + (size_t)y * (1 + w * 3) + 1, rgb + (size_t)y * w * 3, (size_t)w * 3) != 0)
            rc = -1;
    free(raw);
    return rc;
}

static void test_png(void)
{
    static const uint8_t tiny[3 * 2 * 3] = { 255, 0, 0, 0, 255, 0, 0, 0, 255,
                                             1, 2, 3, 4, 5, 6, 7, 8, 9 };
    uint8_t out[256];
    const size_t n = gc_png_encode(tiny, 3, 2, out, sizeof out);
    CHECK(n == gc_png_size(3, 2) && n > 0);
    CHECK(png_check(out, n, tiny, 3, 2) == 0);
    out[40] ^= 1;                                      /* un bit cambiado: el CRC lo ve */
    CHECK(png_check(out, n, tiny, 3, 2) != 0);
    CHECK(gc_png_encode(tiny, 3, 2, out, n - 1) == 0); /* no entra */
    CHECK(gc_png_encode(tiny, 0, 2, out, sizeof out) == 0);

    /* 200x200: más de un bloque de 65 535 bytes */
    const uint32_t w = 200, h = 200;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    const size_t cap = gc_png_size(w, h);
    uint8_t *png = malloc(cap);
    CHECK(rgb && png);
    for (size_t k = 0; k < (size_t)w * h * 3; k++)
        rgb[k] = (uint8_t)(k * 31 + k / 7);
    const size_t m = gc_png_encode(rgb, w, h, png, cap);
    const int ok = m == cap && png_check(png, m, rgb, w, h) == 0;
    free(rgb);
    free(png);
    CHECK(ok);
}

/* ---- los cuadros que avisa la VI ---------------------------------------------------- */

static gc_frame_t seen[8];
static int nseen;

static void record(void *ctx, const gc_frame_t *f)
{
    (void)ctx;
    if (nseen < 8)
        seen[nseen] = *f;
    nseen++;
}

static void test_frames(void)
{
    static const uint32_t spin[] = { 0x38600000u, 0x38630001u, 0x4BFFFFFCu };
    static dolb_t b;
    static uint8_t rgb[640 * 480 * 3];
    static gc_frames_t frames;
    const gc_boot_config_t cfg = { 0, 0 };
    gc_log_set_sink(quiet);
    CHECK(gc_system_init(&sys) == 0);
    dolb_init(&b);
    dolb_code(&b, 0, 0x80003100u, spin, 3);
    dolb_entry(&b, 0x80003100u);
    CHECK(gc_system_boot_dol(&sys, b.buf, b.len, &cfg) == GC_DOL_OK);
    gc_frames_attach(&frames, &sys, rgb, sizeof rgb, record, NULL);
    nseen = 0;

    /* 480i con el XFB en 0x00100000 (como tests/dol/video.c), todo rojo */
    const uint32_t vi = 0xCC002000u;
    gc_write16(&sys.mem, vi + VI_VTR, 0x0F06);
    gc_write32(&sys.mem, vi + VI_VTO, 0x00030018u);
    gc_write32(&sys.mem, vi + VI_VTE, 0x00020019u);
    gc_write16(&sys.mem, vi + VI_CLOCK, 0);
    gc_write32(&sys.mem, vi + VI_TFBL, 0x10008000u);
    gc_write32(&sys.mem, vi + VI_BFBL, 0x10008028u);
    gc_write16(&sys.mem, vi + VI_HSW, 0x2850);
    fill_field(0x100000, 640, 480, 1280, RED);

    const uint64_t field = 525ull * 15444;
    gc_system_run(&sys, 2 * field + 7722);             /* dos campos: el mismo cuadro */
    CHECK(nseen == 1);
    CHECK(seen[0].number == 1 && seen[0].width == 640 && seen[0].height == 480);
    CHECK(row_is(seen[0].rgb, 640, 0, RED) && row_is(seen[0].rgb, 640, 479, RED));
    const uint32_t red_crc = seen[0].crc;
    CHECK_EQ_U64(red_crc, gc_crc32(0, rgb, sizeof rgb));

    fill_field(0x100000, 640, 2, 1280, BLUE);          /* cambia la primera línea de cada campo */
    gc_system_run(&sys, 2 * field);
    CHECK(nseen == 2);
    CHECK(seen[1].number == 2 && seen[1].crc != red_crc && seen[1].field > seen[0].field);
    CHECK(row_is(seen[1].rgb, 640, 0, BLUE) && row_is(seen[1].rgb, 640, 1, BLUE));
    CHECK(row_is(seen[1].rgb, 640, 2, RED));

    gc_frames_attach(&frames, &sys, rgb, 1000, record, NULL);   /* no entra: se cuenta */
    gc_system_run(&sys, field);
    CHECK(nseen == 2 && frames.too_big >= 1);
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

const test_case_t xfb_tests[] = {
    { "crc32: el de zlib, por partes", test_crc32 },
    { "xfb: YUV a RGB (barras al 75 %, extremos, pares de píxeles)", test_yuv_to_rgb },
    { "xfb: cuadro con los dos campos, orden por PRB, un campo, faltantes", test_weave },
    { "png: bloques con CRC, zlib stored con Adler-32, filas sin filtro", test_png },
    { "cuadros: uno por CRC distinto al terminar cada campo, búfer chico", test_frames },
    { NULL, NULL },
};
