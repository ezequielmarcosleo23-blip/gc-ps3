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
 * Ganchos HLE al inicio de funciones del programa emulado (D-033).
 *
 * La primera instrucción de la función se reemplaza por GEKKO_HLE_INST(n).
 * Al llegar ahí, el gancho hace lo suyo (copiar un OSReport al log, anotar un
 * exit) y después se ejecuta la instrucción original: la función corre igual
 * que en el hardware y el programa no nota la diferencia, salvo si se lee su
 * propio código. Es lo que Dolphin llama HookType::Start / HookFlag::Debug.
 *
 * Las direcciones salen de un .map con el formato de Dolphin (lo que escribe
 * "Save Symbol Map" o tools/elf2dol.py), así el mismo archivo sirve en los dos.
 */
#ifndef GCPS3_CORE_HLE_H
#define GCPS3_CORE_HLE_H

#include <stddef.h>
#include <stdint.h>

#include "cpu/gekko.h"

#define GC_HLE_MAX      64
#define GC_HLE_NAME_MAX 32
#define GC_HLE_LINE_MAX 1024

struct gc_system;
typedef void (*gc_hle_fn)(struct gc_system *s);

typedef struct gc_hook {
    uint32_t  addr;
    uint32_t  orig;                     /* la instrucción que había */
    gc_hle_fn fn;
    char      name[GC_HLE_NAME_MAX];
} gc_hook_t;

/* Recibe cada línea que imprime el programa, sin el '\n'. */
typedef void (*gc_report_fn)(void *ctx, const char *line);

typedef struct gc_hle {
    gc_hook_t    hooks[GC_HLE_MAX];
    uint32_t     count;
    gc_report_fn report;                /* NULL: al log del núcleo */
    void        *report_ctx;
    char         line[GC_HLE_LINE_MAX];
    size_t       line_len;
    uint32_t     lines;                 /* líneas entregadas */
    int          exited;                /* el programa llamó a exit */
    uint32_t     exit_code;
} gc_hle_t;

/* Funciones que se reconocen por su nombre en el .map (hle_os.c). */
typedef struct gc_hle_known {
    const char *name;
    gc_hle_fn   fn;
} gc_hle_known_t;
extern const gc_hle_known_t gc_hle_known[];

/* Sin ganchos ni texto pendiente; conserva el destino de las líneas. */
void gc_hle_reset(gc_hle_t *h);

/* Engancha la función en addr. Devuelve el índice, o -1 si no hay lugar o
 * addr no es RAM. Enganchar dos veces la misma dirección no hace nada. */
int gc_hle_patch(struct gc_system *s, uint32_t addr, const char *name, gc_hle_fn fn);

/* Para trazas: si inst es un gancho, la instrucción original; si no, inst. */
uint32_t gc_hle_original(const struct gc_system *s, uint32_t inst);

/* La llamada de la CPU al llegar a GEKKO_HLE_INST(index) (gekko_t.hle). */
uint32_t gc_hle_dispatch(void *ctx, gekko_t *c, uint32_t index);

/* Lee un .map y engancha las funciones de gc_hle_known que estén en .text o
 * .init. Devuelve cuántas enganchó. */
int gc_hle_load_map(struct gc_system *s, const char *text, size_t len);

/* Texto del programa: se arma por líneas y cada '\n' entrega una. */
void gc_hle_output(struct gc_system *s, const char *text, size_t n);

/* Entrega lo que quedó sin '\n'. */
void gc_hle_flush(struct gc_system *s);

#endif
