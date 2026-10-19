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

/* Planificador de eventos: orden, empates, atraso, cancelación y límites. */
#include <string.h>

#include "core/log.h"
#include "core/sched.h"
#include "test.h"

static char order[64];
static int norder;
static uint64_t last_late;

static void mark(void *ctx, uint64_t when, uint64_t late)
{
    (void)when;
    if (norder < (int)sizeof order - 1)
        order[norder++] = *(const char *)ctx;
    order[norder] = '\0';
    last_late = late;
}

static gc_sched_t *self_sched;

/* Agrega otro evento para el mismo ciclo: tiene que salir en el mismo despacho. */
static void chain(void *ctx, uint64_t when, uint64_t late)
{
    static const char c = 'c';
    mark(ctx, when, late);
    gc_sched_add(self_sched, when, "c", mark, (void *)&c);
}

/* Periódico: se reprograma en when + 100 aunque haya llegado tarde. */
static uint64_t periodic_whens[8];
static int nperiodic;
static void periodic(void *ctx, uint64_t when, uint64_t late)
{
    (void)ctx;
    (void)late;
    periodic_whens[nperiodic++] = when;
    if (nperiodic < 8)
        gc_sched_add(self_sched, when + 100, "p", periodic, NULL);
}

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void test_order(void)
{
    static gc_sched_t s;
    static const char a = 'a', b = 'b', d = 'd', e = 'e', x = 'x', y = 'y', z = 'z';
    gc_sched_init(&s);
    self_sched = &s;
    norder = 0;
    order[0] = '\0';

    CHECK(gc_sched_next(&s) == GC_SCHED_NEVER);
    CHECK(gc_sched_add(&s, 5000, "d", mark, (void *)&d) == 0);  /* ocupa el primer lugar */
    CHECK(gc_sched_add(&s, 300, "z", mark, (void *)&z) == 0);
    CHECK(gc_sched_add(&s, 100, "x", mark, (void *)&x) == 0);
    CHECK(gc_sched_add(&s, 200, "a", mark, (void *)&a) == 0);
    CHECK(gc_sched_add(&s, 200, "b", mark, (void *)&b) == 0);   /* mismo ciclo: después de a */
    CHECK(gc_sched_add(&s, 200, "y", chain, (void *)&y) == 0);
    CHECK(gc_sched_cancel(&s, mark, (void *)&d) == 1);
    CHECK(gc_sched_add(&s, 200, "e", mark, (void *)&e) == 0);   /* reusa el lugar, pero es el último */
    CHECK(gc_sched_next(&s) == 100);

    CHECK(gc_sched_dispatch(&s, 99) == 0);
    CHECK(gc_sched_dispatch(&s, 150) == 1);
    CHECK(last_late == 50);
    CHECK(gc_sched_dispatch(&s, 200) == 5);                     /* a, b, y, e y la c que agregó y */
    CHECK(strcmp(order, "xabyec") == 0);
    CHECK(gc_sched_next(&s) == 300);

    CHECK(gc_sched_cancel(&s, mark, (void *)&z) == 1);
    CHECK(gc_sched_cancel(&s, mark, (void *)&z) == 0);
    CHECK(gc_sched_next(&s) == GC_SCHED_NEVER);
    CHECK(s.count == 0);
}

static void test_periodic_and_limits(void)
{
    static gc_sched_t s;
    gc_sched_init(&s);
    self_sched = &s;
    nperiodic = 0;
    CHECK(gc_sched_add(&s, 1000, "p", periodic, NULL) == 0);
    gc_sched_dispatch(&s, 1030);                                 /* tarde: 1000, y 1100 aún no */
    gc_sched_dispatch(&s, 1450);                                 /* 1100, 1200, 1300, 1400 */
    CHECK(nperiodic == 5);
    CHECK(periodic_whens[0] == 1000 && periodic_whens[4] == 1400);
    CHECK(gc_sched_next(&s) == 1500);

    static const char k = 'k';
    gc_log_set_sink(quiet);
    gc_sched_init(&s);
    for (int i = 0; i < GC_SCHED_MAX; i++)
        CHECK(gc_sched_add(&s, (uint64_t)i, "k", mark, (void *)&k) == 0);
    CHECK(gc_sched_add(&s, 0, "k", mark, (void *)&k) == -1);    /* llena */
    CHECK(gc_sched_cancel(&s, mark, (void *)&k) == GC_SCHED_MAX);
    gc_log_set_sink(NULL);
}

const test_case_t sched_tests[] = {
    { "sched: orden, empates en orden de llegada, atraso y cancelacion", test_order },
    { "sched: eventos periodicos y tabla llena", test_periodic_and_limits },
    { NULL, NULL },
};
