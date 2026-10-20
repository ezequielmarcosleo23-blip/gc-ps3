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

/* Formato: PNG (RFC 2083), zlib (RFC 1950) y deflate "stored" (RFC 1951, 3.2.4). */
#include <string.h>

#include "core/crc32.h"
#include "core/png.h"

#define STORED_MAX 65535u                  /* bytes por bloque sin comprimir */

static uint64_t raw_size(uint32_t w, uint32_t h)
{
    return (uint64_t)h * (1 + (uint64_t)w * 3);   /* cada fila: filtro 0 y los píxeles */
}

static uint64_t zlib_size(uint64_t raw)
{
    const uint64_t blocks = raw ? (raw + STORED_MAX - 1) / STORED_MAX : 1;
    return 2 + blocks * 5 + raw + 4;              /* cabecera, bloques, Adler-32 */
}

size_t gc_png_size(uint32_t w, uint32_t h)
{
    const uint64_t n = 8 + (12 + 13) + (12 + zlib_size(raw_size(w, h))) + 12;
    return n > 0x7FFFFFFFu ? 0 : (size_t)n;
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

/* Cierra un bloque: largo delante y CRC de tipo + datos detrás */
static uint8_t *chunk(uint8_t *start, const char *type, uint32_t len)
{
    put32(start, len);
    memcpy(start + 4, type, 4);
    return put32(start + 8 + len, gc_crc32(0, start + 4, 4 + (size_t)len));
}

/* La corriente de deflate, partida en bloques "stored" */
typedef struct stream {
    uint8_t *p;
    uint64_t done, total;
    uint32_t a, b;                         /* Adler-32 */
} stream_t;

static void emit(stream_t *s, const uint8_t *data, uint64_t n)
{
    while (n) {
        if (s->done % STORED_MAX == 0) {   /* cabecera de un bloque nuevo */
            const uint64_t left = s->total - s->done;
            const uint32_t len = (uint32_t)(left > STORED_MAX ? STORED_MAX : left);
            *s->p++ = left <= STORED_MAX ? 1 : 0;           /* BFINAL, BTYPE = 00 */
            *s->p++ = (uint8_t)len;
            *s->p++ = (uint8_t)(len >> 8);
            *s->p++ = (uint8_t)~len;
            *s->p++ = (uint8_t)(~len >> 8);
        }
        uint64_t take = STORED_MAX - s->done % STORED_MAX;
        if (take > n)
            take = n;
        memcpy(s->p, data, (size_t)take);
        for (uint64_t k = 0; k < take; k++) {
            s->a = (s->a + data[k]) % 65521u;
            s->b = (s->b + s->a) % 65521u;
        }
        s->p += take;
        s->done += take;
        data += take;
        n -= take;
    }
}

size_t gc_png_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap)
{
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    const size_t size = gc_png_size(w, h);
    if (size == 0 || size > cap || w == 0 || h == 0)
        return 0;

    uint8_t *p = out;
    memcpy(p, signature, 8);
    p += 8;

    uint8_t *ihdr = p;                             /* IHDR: 8 bits, RGB, sin entrelazado */
    uint8_t *d = put32(put32(ihdr + 8, w), h);
    d[0] = 8;
    d[1] = 2;
    d[2] = d[3] = d[4] = 0;
    p = chunk(ihdr, "IHDR", 13);

    uint8_t *idat = p;
    stream_t s = { idat + 8 + 2, 0, raw_size(w, h), 1, 0 };
    idat[8] = 0x78;                                /* zlib: deflate, ventana de 32 KB */
    idat[9] = 0x01;                                /* sin diccionario; (0x7801 % 31) == 0 */
    static const uint8_t filter_none = 0;
    for (uint32_t y = 0; y < h; y++) {
        emit(&s, &filter_none, 1);
        emit(&s, rgb + (size_t)y * w * 3, (uint64_t)w * 3);
    }
    s.p = put32(s.p, s.b << 16 | s.a);
    p = chunk(idat, "IDAT", (uint32_t)(s.p - (idat + 8)));

    p = chunk(p, "IEND", 0);
    return (size_t)(p - out);
}
