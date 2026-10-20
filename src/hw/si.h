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
 * Serial Interface (0x0C006400): los 4 puertos de mandos.
 *
 * Como Source/Core/Core/HW/SI/SI.cpp y SI_DeviceGCController.cpp de Dolphin
 * (commit cf5cb7ef): un búfer de 128 bytes para comandos sueltos (COMCSR,
 * la respuesta está al instante), el sondeo que dispara la VI cada X líneas
 * (gc_si_poll) y SISR con los errores por puerto. En el puerto 1 hay un mando
 * estándar; los otros tres están vacíos (sin respuesta), como en Dolphin por
 * omisión. El estado del mando lo pone la plataforma en gc_si_t.pad; hasta
 * que haya entrada de la PC o de la PS3, sin botones y con las palancas al
 * centro.
 */
#ifndef GCPS3_HW_SI_H
#define GCPS3_HW_SI_H

#include <stdint.h>

#define GC_SI_PORTS    4
#define GC_SI_BUF_SIZE 128

/* Registros (desplazamiento desde 0x0C006400) */
enum {
    SI_OUT0 = 0x00, SI_IN0_HI = 0x04, SI_IN0_LO = 0x08,   /* canal n: + 0x0C * n */
    SI_POLL = 0x30, SI_COMCSR = 0x34, SI_SR = 0x38, SI_EXILK = 0x3C, SI_BUF = 0x80,
};

/* COMCSR */
#define SI_COMCSR_TSTART  0x00000001u
#define SI_COMCSR_RDSTINTMSK 0x08000000u
#define SI_COMCSR_RDSTINT 0x10000000u
#define SI_COMCSR_COMERR  0x20000000u
#define SI_COMCSR_TCINTMSK 0x40000000u
#define SI_COMCSR_TCINT   0x80000000u

/* SISR: un byte por canal, el canal 0 arriba */
#define SI_SR_NOREP(c)  (0x08000000u >> (8 * (c)))
#define SI_SR_RDST(c)   (0x20000000u >> (8 * (c)))
#define SI_SR_WR        0x80000000u

/* Respuesta al comando 0x00 (estado): mando estándar de GameCube */
#define SI_ID_GC_CONTROLLER 0x09000000u

/* Botones (los de la palabra alta de la respuesta) */
#define GC_PAD_USE_ORIGIN 0x0080u

typedef struct gc_pad {
    int      connected;
    uint16_t buttons;
    uint8_t  stick_x, stick_y, sub_x, sub_y;       /* 0x80 = centro */
    uint8_t  trig_l, trig_r, analog_a, analog_b;
} gc_pad_t;

struct gc_system;

typedef struct gc_si {
    struct gc_system *sys;
    gc_pad_t pad[GC_SI_PORTS];
    uint8_t  mode[GC_SI_PORTS];          /* formato de la respuesta (comando 0x40) */
    uint8_t  origin[GC_SI_PORTS][10];    /* respuesta a 0x41/0x42 */
    uint32_t out[GC_SI_PORTS], in_hi[GC_SI_PORTS], in_lo[GC_SI_PORTS];
    uint32_t poll, comcsr, sr, exilk;
    uint8_t  buf[GC_SI_BUF_SIZE];
} gc_si_t;

void gc_si_init(gc_si_t *si, struct gc_system *sys);
void gc_si_reset(gc_si_t *si);

/* El sondeo de los puertos (lo llama la VI): IN_HI/IN_LO y RDST. */
void gc_si_poll(gc_si_t *si);

/* Cada cuántas líneas sondea (campo X de SIPOLL). */
uint32_t gc_si_poll_lines(const gc_si_t *si);

#endif
