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
#include <string.h>

#include "core/xfb.h"

static uint8_t clamp(int32_t v)
{
    if (v < 0)
        return 0;                          /* sin desplazar negativos (depende del compilador) */
    v >>= 8;
    return (uint8_t)(v > 255 ? 255 : v);
}

void gc_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t rgb[3])
{
    const int32_t c = 298 * ((int32_t)y - 16), d = (int32_t)u - 128, e = (int32_t)v - 128;
    rgb[0] = clamp(c + 409 * e + 128);
    rgb[1] = clamp(c - 100 * d - 208 * e + 128);
    rgb[2] = clamp(c + 516 * d + 128);
}

void gc_xfb_line_to_rgb(const uint8_t *yuyv, uint32_t width, uint8_t *rgb)
{
    for (uint32_t x = 0; x + 1 < width; x += 2, yuyv += 4, rgb += 6) {
        gc_yuv_to_rgb(yuyv[0], yuyv[1], yuyv[3], rgb);
        gc_yuv_to_rgb(yuyv[2], yuyv[1], yuyv[3], rgb + 3);
    }
}

/* Los dos campos en el orden de la pantalla; si falta uno, el otro hace de los dos */
static int fields(const gc_vi_t *vi, const gc_vi_field_t **top, const gc_vi_field_t **bottom)
{
    const gc_vi_field_t *odd = &vi->field[0], *even = &vi->field[1];
    if (!odd->xfb && !even->xfb)
        return -1;
    if (!odd->xfb)
        odd = even;
    if (!even->xfb)
        even = odd;
    const int odd_top = gc_vi_odd_on_top(vi);
    *top = odd_top ? odd : even;
    *bottom = odd_top ? even : odd;
    return 0;
}

size_t gc_xfb_frame_size(const gc_vi_t *vi, uint32_t *width, uint32_t *height)
{
    const gc_vi_field_t *top, *bottom;
    uint32_t w = 0, h = 0;
    if (fields(vi, &top, &bottom) == 0) {
        w = top->width > bottom->width ? top->width : bottom->width;
        h = 2 * (top->height > bottom->height ? top->height : bottom->height);
    }
    if (w == 0 || h == 0)
        w = h = 0;
    if (width)
        *width = w;
    if (height)
        *height = h;
    return (size_t)w * h * 3;
}

int gc_xfb_frame(gc_mem_t *m, const gc_vi_t *vi, uint8_t *rgb, size_t cap)
{
    uint32_t w, h;
    const size_t size = gc_xfb_frame_size(vi, &w, &h);
    const gc_vi_field_t *top, *bottom;
    if (size == 0 || size > cap || fields(vi, &top, &bottom) != 0)
        return -1;
    for (uint32_t line = 0; line < h; line++) {
        const gc_vi_field_t *f = (line & 1) ? bottom : top;
        const uint32_t k = line / 2;
        uint8_t *out = rgb + (size_t)line * w * 3;
        memset(out, 0, (size_t)w * 3);
        if (k >= f->height || f->width == 0)
            continue;
        const uint8_t *src = gc_ram_ptr(m, f->xfb + k * f->stride, f->width * 2);
        if (src)
            gc_xfb_line_to_rgb(src, f->width, out);
    }
    return 0;
}
