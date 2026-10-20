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

#include "core/crc32.h"
#include "core/frames.h"
#include "core/system.h"
#include "core/xfb.h"

static void on_field(void *ctx, const gc_vi_field_t *field)
{
    gc_frames_t *f = ctx;
    gc_system_t *s = f->sys;
    uint32_t w, h;
    (void)field;
    const size_t size = gc_xfb_frame_size(&s->vi, &w, &h);
    if (size == 0)
        return;
    if (size > f->cap || gc_xfb_frame(&s->mem, &s->vi, f->rgb, f->cap) != 0) {
        f->too_big++;
        return;
    }
    const uint32_t crc = gc_crc32(0, f->rgb, size);
    if (f->last.number && crc == f->last.crc && w == f->last.width && h == f->last.height)
        return;
    f->last.number++;
    f->last.field = s->vi.fields;
    f->last.width = w;
    f->last.height = h;
    f->last.crc = crc;
    f->last.rgb = f->rgb;
    if (f->fn)
        f->fn(f->ctx, &f->last);
}

void gc_frames_reset(gc_frames_t *f)
{
    memset(&f->last, 0, sizeof f->last);
    f->too_big = 0;
}

void gc_frames_attach(gc_frames_t *f, struct gc_system *sys, uint8_t *rgb, size_t cap,
                      gc_frame_fn fn, void *ctx)
{
    f->sys = sys;
    f->rgb = rgb;
    f->cap = cap;
    f->fn = fn;
    f->ctx = ctx;
    gc_frames_reset(f);
    sys->vi.on_field = on_field;
    sys->vi.on_field_ctx = f;
}
