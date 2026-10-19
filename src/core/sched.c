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

#include "core/log.h"
#include "core/sched.h"

void gc_sched_init(gc_sched_t *s)
{
    memset(s, 0, sizeof *s);
}

int gc_sched_add(gc_sched_t *s, uint64_t when, const char *name, gc_event_fn fn, void *ctx)
{
    for (int k = 0; k < GC_SCHED_MAX; k++) {
        gc_event_t *e = &s->ev[k];
        if (e->used)
            continue;
        e->when = when;
        e->seq = s->seq++;
        e->fn = fn;
        e->ctx = ctx;
        e->name = name;
        e->used = 1;
        s->count++;
        return 0;
    }
    GC_LOGE("sched: tabla llena, se pierde el evento %s", name ? name : "?");
    return -1;
}

int gc_sched_cancel(gc_sched_t *s, gc_event_fn fn, void *ctx)
{
    int n = 0;
    for (int k = 0; k < GC_SCHED_MAX; k++) {
        gc_event_t *e = &s->ev[k];
        if (e->used && e->fn == fn && e->ctx == ctx) {
            e->used = 0;
            s->count--;
            n++;
        }
    }
    return n;
}

/* El primero en salir: menor `when`, y entre iguales el más viejo. */
static gc_event_t *first(gc_sched_t *s)
{
    gc_event_t *best = NULL;
    for (int k = 0; k < GC_SCHED_MAX; k++) {
        gc_event_t *e = &s->ev[k];
        if (e->used && (!best || e->when < best->when || (e->when == best->when && e->seq < best->seq)))
            best = e;
    }
    return best;
}

uint64_t gc_sched_next(const gc_sched_t *s)
{
    uint64_t next = GC_SCHED_NEVER;
    for (int k = 0; k < GC_SCHED_MAX; k++)
        if (s->ev[k].used && s->ev[k].when < next)
            next = s->ev[k].when;
    return next;
}

/* Un evento que se reprograma siempre para el ciclo actual colgaría el bucle. */
#define DISPATCH_MAX (GC_SCHED_MAX * 64)

int gc_sched_dispatch(gc_sched_t *s, uint64_t now)
{
    int ran = 0;
    for (;;) {
        gc_event_t *e = first(s);
        if (!e || e->when > now)
            return ran;
        if (ran == DISPATCH_MAX) {
            GC_LOGE("sched: %d eventos en el ciclo %llu; el resto queda para el siguiente tramo",
                    ran, (unsigned long long)now);
            return ran;
        }
        const gc_event_t ev = *e;          /* se libera antes: puede reprogramarse */
        e->used = 0;
        s->count--;
        ev.fn(ev.ctx, ev.when, now - ev.when);
        ran++;
    }
}
