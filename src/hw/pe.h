/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008 Dolphin Emulator Project
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
 * Pixel Engine (0x0C001000): los registros que ve la CPU (configuración de Z y
 * alfa, control, token) y las dos señales que levanta la GPU, el token
 * (BP 0x47 y 0x48) y "draw done" (BP 0x45), con sus interrupciones al PI.
 *
 * Como Source/Core/VideoCommon/PixelEngine.cpp de Dolphin (commit cf5cb7ef)
 * con un solo hilo: la GPU no cambia el token en el acto, agenda un evento
 * max(500, ciclos acumulados) después; si ya había uno pendiente, el nuevo se
 * junta con ese (token más nuevo, interrupciones sumadas). Escribir en el
 * control el bit de TOKEN o FINISH baja esa señal.
 */
#ifndef GCPS3_HW_PE_H
#define GCPS3_HW_PE_H

#include <stdint.h>

/* Registros (desplazamiento desde 0x0C001000) */
enum {
    PE_ZCONF = 0x00, PE_ALPHACONF = 0x02, PE_DSTALPHACONF = 0x04, PE_ALPHAMODE = 0x06,
    PE_ALPHAREAD = 0x08, PE_CTRL = 0x0A, PE_TOKEN = 0x0E,
    PE_BBOX_FIRST = 0x10, PE_BBOX_LAST = 0x16, PE_PERF_FIRST = 0x18, PE_PERF_LAST = 0x2E,
};

/* Control */
enum {
    PE_CTRL_TOKEN_ENABLE = 0x01, PE_CTRL_FINISH_ENABLE = 0x02,
    PE_CTRL_TOKEN = 0x04, PE_CTRL_FINISH = 0x08,   /* solo se escriben: bajan la señal */
};

#define GC_PE_MIN_DELAY 500   /* "Dolphin's single-core gpu timings are way too fast" */

struct gc_system;

typedef struct gc_pe {
    struct gc_system *sys;
    uint16_t zconf, alphaconf, dstalphaconf, alphamode, alpharead;
    uint16_t ctrl;
    uint16_t token, token_pending;
    int      token_irq_pending, finish_irq_pending;
    int      event_raised;
    int      signal_token, signal_finish;
    uint32_t tokens, finishes;      /* señales que llegaron (diagnóstico) */
    uint32_t bad;
} gc_pe_t;

void gc_pe_init(gc_pe_t *pe, struct gc_system *sys);
void gc_pe_reset(gc_pe_t *pe);

/* Desde la GPU (BP 0x47/0x48 y 0x45 = 2): cycles = ciclos que ya llevaba el
 * lector de comandos en este bloque. */
void gc_pe_set_token(gc_pe_t *pe, uint16_t token, int irq, uint32_t cycles);
void gc_pe_set_finish(gc_pe_t *pe, uint32_t cycles);

#endif
