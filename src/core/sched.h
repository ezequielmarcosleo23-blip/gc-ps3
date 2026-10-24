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
 * Planificador de eventos en ciclos de CPU (el CoreTiming de Dolphin, en
 * chico): el hardware pide "llamame en el ciclo N" (retrazo de video, fin de
 * un DMA...) y gc_system_run corre la CPU en tramos hasta el próximo evento.
 *
 * Pocos eventos a la vez (decenas): una tabla fija recorrida entera es más
 * simple que un heap y alcanza. Dos eventos del mismo ciclo salen en el orden
 * en que se agregaron.
 */
#ifndef GCPS3_CORE_SCHED_H
#define GCPS3_CORE_SCHED_H

#include <stdint.h>

#define GC_SCHED_MAX   32
#define GC_SCHED_NEVER UINT64_MAX

/* late = ciclos de atraso respecto de `when` (>= 0): un evento periódico se
 * reprograma en when + periodo para no acumular el atraso. */
typedef void (*gc_event_fn)(void *ctx, uint64_t when, uint64_t late);

typedef struct gc_event {
    uint64_t    when;
    uint64_t    seq;            /* orden de llegada, para los empates */
    gc_event_fn fn;
    void       *ctx;
    const char *name;
    int         used;
} gc_event_t;

typedef struct gc_sched {
    gc_event_t ev[GC_SCHED_MAX];
    uint64_t   seq;
    uint32_t   count;
    /* Fin del tramo que está corriendo la CPU (gekko_t.run_end), o NULL. Un
     * evento agregado para antes lo acorta: así un registro que agenda algo a
     * pocos ciclos (GX: 0, 500, 1000) lo ve atendido a tiempo, entre una
     * instrucción y la siguiente, como en Dolphin. */
    uint64_t  *limit;
} gc_sched_t;

/* Vacía la tabla (limit queda en NULL). */
void gc_sched_init(gc_sched_t *s);

/* Programa fn(ctx) para el ciclo `when` (y acorta el tramo de la CPU si hace
 * falta). 0 o -1 si la tabla está llena. */
int gc_sched_add(gc_sched_t *s, uint64_t when, const char *name, gc_event_fn fn, void *ctx);

/* Quita todos los eventos pendientes con esa función y contexto. Devuelve cuántos. */
int gc_sched_cancel(gc_sched_t *s, gc_event_fn fn, void *ctx);

/* Ciclo del próximo evento, o GC_SCHED_NEVER. */
uint64_t gc_sched_next(const gc_sched_t *s);

/* Ejecuta, en orden, los eventos con when <= now (también los que agreguen
 * los propios eventos para entonces). Devuelve cuántos corrió. */
int gc_sched_dispatch(gc_sched_t *s, uint64_t now);

#endif
