/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008-2009 Dolphin Emulator Project
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
 * Processor Interface (0x0C003000): junta las interrupciones de todo el
 * hardware y le maneja a la CPU la línea de interrupción externa. Cada bloque
 * prende o apaga su causa en INTSR; si alguna causa está en INTMR, la línea
 * queda alta (la CPU la toma cuando tiene MSR[EE]).
 *
 * Como Source/Core/Core/HW/ProcessorInterface.cpp de Dolphin (commit
 * cf5cb7ef): escribir 1 en un bit de INTSR lo borra; el bit 16 (RSWST) es el
 * estado del botón de reset (1 = suelto) y no se enmascara.
 */
#ifndef GCPS3_HW_PI_H
#define GCPS3_HW_PI_H

#include <stdint.h>

/* Causas en INTSR / INTMR */
enum {
    PI_INT_PI = 0x0001,        /* error de GP */
    PI_INT_RSW = 0x0002,       /* botón de reset */
    PI_INT_DI = 0x0004,
    PI_INT_SI = 0x0008,
    PI_INT_EXI = 0x0010,
    PI_INT_AI = 0x0020,
    PI_INT_DSP = 0x0040,
    PI_INT_MEM = 0x0080,
    PI_INT_VI = 0x0100,
    PI_INT_PE_TOKEN = 0x0200,
    PI_INT_PE_FINISH = 0x0400,
    PI_INT_CP = 0x0800,
    PI_INT_DEBUG = 0x1000,
    PI_INT_HSP = 0x2000,
    PI_RSW_STATE = 0x10000,    /* 1 = botón de reset suelto */
};

/* Registros (desplazamiento desde 0x0C003000) */
enum {
    PI_INTSR = 0x00, PI_INTMR = 0x04, PI_FIFO_BASE = 0x0C, PI_FIFO_END = 0x10,
    PI_FIFO_WPTR = 0x14, PI_FIFO_RESET = 0x18, PI_ERROR_CAUSE = 0x1C, PI_ERROR_ADDR = 0x20,
    PI_RESET_CODE = 0x24, PI_UNKNOWN = 0x28, PI_FLIPPER_REV = 0x2C, PI_BUS_STRENGTH = 0x30,
};

#define PI_FLIPPER_REV_C 0x246500B1u   /* la revisión de las consolas de venta */

struct gc_system;

typedef struct gc_pi {
    struct gc_system *sys;
    uint32_t cause;
    uint32_t mask;
    uint32_t fifo_base, fifo_end, fifo_wptr;
    uint32_t error_cause, error_addr;
    uint32_t reset_code;
    uint32_t unknown;
    uint32_t bus_strength;
} gc_pi_t;

/* Estado de encendido y registro en el mapa de registros. */
void gc_pi_init(gc_pi_t *pi, struct gc_system *sys);
void gc_pi_reset(gc_pi_t *pi);

/* Un bloque prende (set = 1) o apaga su causa. */
void gc_pi_set_interrupt(gc_pi_t *pi, uint32_t cause, int set);

#endif
