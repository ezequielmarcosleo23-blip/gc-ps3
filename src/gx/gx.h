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
 * El lado de la GPU (Fase 2, etapa A): lee el FIFO de GX de la RAM, decodifica
 * los comandos y guarda el estado de BP, CP y XF. Las primitivas todavía no se
 * dibujan: se saltan según el tamaño de vértice que dan VCD y VAT.
 *
 * El tiempo es el de Dolphin (commit cf5cb7ef) con un solo hilo
 * (VideoCommon/Fifo.cpp y OpcodeDecoding.cpp):
 *   - cuando llega algo al FIFO, la GPU se despierta GC_GX_SLOT ciclos después
 *     (evento SyncGPU) y lee bloques de 32 bytes mientras tenga presupuesto;
 *   - cada comando cuesta lo que dice OpcodeDecoding (BP y CP 12, XF 18 + 6
 *     por palabra, ...); si se pasa, la deuda queda para la vez siguiente y el
 *     evento vuelve deuda + GC_GX_SLOT ciclos después;
 *   - leer ciertos registros del CP corre la GPU en el acto con GC_GX_SLOT
 *     ciclos (gc_gx_sync_for_register_access).
 * Un comando que no entra entero queda en el búfer de video hasta el bloque
 * siguiente, como en Dolphin.
 */
#ifndef GCPS3_GX_GX_H
#define GCPS3_GX_GX_H

#include <stddef.h>
#include <stdint.h>

#define GC_GX_SLOT       1000             /* GPU_TIME_SLOT_SIZE */
#define GC_GX_VBUF_SIZE  (2u << 20)       /* FIFO_SIZE de Dolphin: lo más largo que espera un comando */

/* Memoria de XF que se ve por comandos: matrices, luces y registros (XFMemory) */
#define GC_XF_REGS_START 0x1000u
#define GC_XF_REGS_END   0x1058u
#define GC_XF_SIZE       GC_XF_REGS_END

/* Registros de CP que se cargan por el FIFO (CPMemory.h) */
enum {
    GX_CP_MATINDEX_A = 0x30, GX_CP_MATINDEX_B = 0x40, GX_CP_VCD_LO = 0x50, GX_CP_VCD_HI = 0x60,
    GX_CP_VAT_A = 0x70, GX_CP_VAT_B = 0x80, GX_CP_VAT_C = 0x90,
    GX_CP_ARRAY_BASE = 0xA0, GX_CP_ARRAY_STRIDE = 0xB0,
};

/* Registros de BP con efectos en esta etapa */
enum {
    GX_BP_DRAWDONE = 0x45, GX_BP_TOKEN = 0x47, GX_BP_TOKEN_INT = 0x48, GX_BP_MASK = 0xFE,
};

/* Comandos del FIFO */
enum {
    GX_NOP = 0x00, GX_LOAD_CP = 0x08, GX_LOAD_XF = 0x10, GX_LOAD_INDX_A = 0x20,
    GX_LOAD_INDX_B = 0x28, GX_LOAD_INDX_C = 0x30, GX_LOAD_INDX_D = 0x38, GX_CALL_DL = 0x40,
    GX_METRICS = 0x44, GX_INVL_VC = 0x48, GX_LOAD_BP = 0x61,
    GX_PRIMITIVE_FIRST = 0x80, GX_PRIMITIVE_LAST = 0xBF,
};

/* Estado de CP que llega por el FIFO (CPState de Dolphin) */
typedef struct gc_gx_cp_state {
    uint32_t matindex_a, matindex_b;
    uint32_t vcd_lo, vcd_hi;
    uint32_t vat[8][3];               /* grupos A, B y C de cada formato */
    uint32_t array_base[16];
    uint32_t array_stride[16];
} gc_gx_cp_state_t;

struct gc_system;

typedef struct gc_gx {
    struct gc_system *sys;

    /* Búfer de video: lo leído del FIFO que todavía no se decodificó */
    uint8_t  *vbuf;                   /* GC_GX_VBUF_SIZE bytes */
    uint32_t  vrd, vwr;

    /* Tiempo (FifoManager de Dolphin) */
    int32_t   sync_ticks;             /* deuda de la vez anterior (<= 0) */
    int       suspended;              /* sin evento SyncGPU pendiente */
    int32_t   event_ticks;            /* presupuesto del evento pendiente */

    /* Lector de comandos (RunCallback) */
    uint32_t  cycles;                 /* ciclos del bloque en curso */
    int       in_dl;

    /* Estado */
    uint32_t  bp[256];
    gc_gx_cp_state_t cp;
    uint32_t  xf[GC_XF_SIZE];

    /* Contadores (log y pruebas) */
    uint64_t  commands;               /* comandos decodificados */
    uint64_t  primitives, vertices;   /* saltados hasta la etapa B */
    uint64_t  display_lists;
    uint32_t  unknown;                /* opcodes desconocidos */
    uint32_t  warnings;               /* avisos del log (se limitan) */
    uint8_t   bp_noted[256];          /* registros de BP con efecto pendiente ya avisados */
} gc_gx_t;

/* Reserva el búfer de video. 0, o -1 sin memoria. */
int  gc_gx_init(gc_gx_t *gx, struct gc_system *sys);
void gc_gx_free(gc_gx_t *gx);

/* Estado de encendido (BPInit, CPState y xfmem en cero, GPU sin evento). */
void gc_gx_reset(gc_gx_t *gx);

/* ---- tiempo (fifo.c) ---- */

/* Despierta la GPU: agenda SyncGPU si no había uno (Fifo::RunGpu). */
void gc_gx_run_gpu(gc_gx_t *gx);

/* La CPU lee un registro que depende de la GPU: la corre GC_GX_SLOT ciclos ya. */
void gc_gx_sync_for_register_access(gc_gx_t *gx);

/* Procesa el FIFO con `ticks` ciclos de presupuesto (RunGpuOnCpu). Devuelve
 * -1 si quedó ociosa o cuántos ciclos esperar hasta la próxima vez. */
int32_t gc_gx_run_gpu_on_cpu(gc_gx_t *gx, int32_t ticks);

/* Descarta lo que quedaba a medio leer (PI_FIFO_RESET). */
void gc_gx_reset_video_buffer(gc_gx_t *gx);

/* ---- lector de comandos (opcode.c) ---- */

/* Decodifica comandos enteros de data[0..avail) sumando sus ciclos a
 * gx->cycles. Devuelve los bytes consumidos (un comando incompleto queda). */
uint32_t gc_gx_run_commands(gc_gx_t *gx, const uint8_t *data, uint32_t avail);

/* ---- estado (bp.c, cpmem.c, xf.c) ---- */

void     gc_gx_load_bp(gc_gx_t *gx, uint8_t reg, uint32_t value);
void     gc_gx_load_cp(gc_gx_t *gx, uint8_t cmd, uint32_t value);
void     gc_gx_load_xf(gc_gx_t *gx, uint16_t base, uint32_t count, const uint8_t *data);
void     gc_gx_load_xf_indexed(gc_gx_t *gx, uint32_t array, uint32_t index, uint16_t address,
                               uint32_t size);
uint32_t gc_gx_vertex_size(gc_gx_t *gx, uint32_t vat);

/* La memoria de CP en el orden de sus comandos (FillCPMemoryArray): 256 palabras. */
void     gc_gx_cp_memory(const gc_gx_t *gx, uint32_t out[256]);

/* ---- estado completo para comparar con Dolphin (estado.c) ---- */

#define GC_GX_ESTADO_VERSION 1

/* Escribe el estado en texto, línea por línea (sin '\n'): punteros del FIFO
 * del CP y del PI, bytes en el gather pipe, y las palabras distintas de cero
 * de BP, CP y XF. El parche de Dolphin escribe lo mismo (docs/traza-dolphin.md). */
void gc_gx_estado(const struct gc_system *s, void (*line)(void *ctx, const char *text), void *ctx);

#endif
