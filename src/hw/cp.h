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
 * Registros del Command Processor (0x0C000000): el FIFO de GX que lee la GPU
 * (base, fin, marcas alta y baja, distancia, punteros, breakpoint), control,
 * estado y la interrupción del CP al PI.
 *
 * Como Source/Core/VideoCommon/CommandProcessor.cpp de Dolphin (commit
 * cf5cb7ef) con un solo hilo:
 *   - registros de 16 bits; las mitades LO (dirección menor) llevan los bits
 *     bajos y solo guardan 0xFFE0, las HI solo 0x03FF;
 *   - leer el estado, la mitad alta de la distancia o del puntero de lectura
 *     (y escribir estas dos) corre antes la GPU 1000 ciclos
 *     (gc_gx_sync_for_register_access);
 *   - las interrupciones cambian en el acto, sin evento;
 *   - el bloque de END es parte del FIFO: después de él se vuelve a la base.
 */
#ifndef GCPS3_HW_CP_H
#define GCPS3_HW_CP_H

#include <stdint.h>

/* Registros (desplazamiento desde 0x0C000000) */
enum {
    CP_SR = 0x00, CP_CR = 0x02, CP_CLEAR = 0x04, CP_PERF_SELECT = 0x06, CP_UNK_0A = 0x0A,
    CP_FIFO_BASE_LO = 0x20, CP_FIFO_BASE_HI = 0x22, CP_FIFO_END_LO = 0x24, CP_FIFO_END_HI = 0x26,
    CP_FIFO_HI_WM_LO = 0x28, CP_FIFO_HI_WM_HI = 0x2A, CP_FIFO_LO_WM_LO = 0x2C, CP_FIFO_LO_WM_HI = 0x2E,
    CP_FIFO_DIST_LO = 0x30, CP_FIFO_DIST_HI = 0x32, CP_FIFO_WPTR_LO = 0x34, CP_FIFO_WPTR_HI = 0x36,
    CP_FIFO_RPTR_LO = 0x38, CP_FIFO_RPTR_HI = 0x3A, CP_FIFO_BP_LO = 0x3C, CP_FIFO_BP_HI = 0x3E,
    CP_METRICS_FIRST = 0x40, CP_METRICS_LAST = 0x5A, CP_CLKS_PER_VTX_OUT = 0x64,
};

/* Estado (SR) */
enum {
    CP_SR_OVERFLOW = 0x01, CP_SR_UNDERFLOW = 0x02, CP_SR_READ_IDLE = 0x04,
    CP_SR_CMD_IDLE = 0x08, CP_SR_BREAKPOINT = 0x10,
};

/* Control (CR) */
enum {
    CP_CR_READ = 0x01, CP_CR_BP_ENABLE = 0x02, CP_CR_OVF_INT = 0x04, CP_CR_UNDF_INT = 0x08,
    CP_CR_LINK = 0x10, CP_CR_BP_INT = 0x20,
};

#define GC_CP_ADDR_MASK 0x03FFFFFFu   /* GetPhysicalAddressMask (GameCube) */
#define GC_CP_LO_MASK   0xFFE0u
#define GC_CP_HI_MASK   0x03FFu

/* El FIFO que lee la GPU (SCPFifoStruct de Dolphin) */
typedef struct gc_cp_fifo {
    uint32_t base, end;
    uint32_t hi_wm, lo_wm;
    uint32_t distance;
    uint32_t wptr, rptr;
    uint32_t bp;
    uint8_t  read_enable, bp_enable, bp_int, breakpoint;
    uint8_t  lo_int, hi_int, lo, hi;
    uint8_t  link;
} gc_cp_fifo_t;

struct gc_system;

typedef struct gc_cp {
    struct gc_system *sys;
    gc_cp_fifo_t fifo;
    uint16_t status, ctrl, clear;
    uint16_t perf_select, unk_0a;
    int      irq_set;                 /* la causa CP prendida en el PI */
    uint32_t bad;                     /* accesos a registros que no existen */
} gc_cp_t;

void gc_cp_init(gc_cp_t *cp, struct gc_system *sys);
void gc_cp_reset(gc_cp_t *cp);

/* Un bloque del gather pipe llegó a la RAM (GatherPipeBursted). */
void gc_cp_gather_pipe_bursted(gc_cp_t *cp);

/* Marcas, breakpoint e interrupción después de que la GPU leyó (SetCPStatusFromGPU). */
void gc_cp_status_from_gpu(gc_cp_t *cp);

/* PI_FIFO_RESET: enlazado, sin leer ni interrupciones, marcas por omisión (ResetFifo). */
void gc_cp_reset_fifo(gc_cp_t *cp);

/* La GPU está parada en el breakpoint (Fifo::AtBreakpoint). */
static inline int gc_cp_at_breakpoint(const gc_cp_t *cp)
{
    return cp->fifo.bp_enable && cp->fifo.rptr == cp->fifo.bp;
}

#endif
