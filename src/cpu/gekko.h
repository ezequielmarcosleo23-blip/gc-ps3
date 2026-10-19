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
 * Gekko (PowerPC 750CXe con paired singles): estado de la CPU y la API del
 * intérprete. El mismo código corre en la PC (runner y pruebas) y en el PPE.
 *
 * Bits como en los manuales de IBM, numerados desde el más significativo (bit 0
 * = 0x80000000). Cada instrucción cuenta como 1 ciclo de CPU (486 MHz); el
 * timebase avanza 1 cada 12 ciclos (40,5 MHz) y el decrementador con él.
 */
#ifndef GCPS3_CPU_GEKKO_H
#define GCPS3_CPU_GEKKO_H

#include <stdint.h>

#include "core/mem.h"

/* MSR */
#define MSR_POW  0x00040000u
#define MSR_ILE  0x00010000u
#define MSR_EE   0x00008000u
#define MSR_PR   0x00004000u
#define MSR_FP   0x00002000u
#define MSR_ME   0x00001000u
#define MSR_FE0  0x00000800u
#define MSR_SE   0x00000400u
#define MSR_BE   0x00000200u
#define MSR_FE1  0x00000100u
#define MSR_IP   0x00000040u
#define MSR_IR   0x00000020u
#define MSR_DR   0x00000010u
#define MSR_RI   0x00000002u
#define MSR_LE   0x00000001u

/* XER */
#define XER_SO   0x80000000u
#define XER_OV   0x40000000u
#define XER_CA   0x20000000u
#define XER_BC   0x0000007Fu

/* SPR que usa el intérprete (número de SPR) */
enum {
    SPR_XER = 1, SPR_LR = 8, SPR_CTR = 9, SPR_DSISR = 18, SPR_DAR = 19, SPR_DEC = 22,
    SPR_SDR1 = 25, SPR_SRR0 = 26, SPR_SRR1 = 27, SPR_TBL_R = 268, SPR_TBU_R = 269,
    SPR_SPRG0 = 272, SPR_EAR = 282, SPR_TBL_W = 284, SPR_TBU_W = 285, SPR_PVR = 287,
    SPR_IBAT0U = 528, SPR_DBAT3L = 543, SPR_GQR0 = 912, SPR_HID2 = 920, SPR_WPAR = 921,
    SPR_DMAU = 922, SPR_DMAL = 923, SPR_HID0 = 1008, SPR_HID1 = 1009, SPR_L2CR = 1017,
};

#define GEKKO_PVR          0x00083214u
#define GEKKO_TB_DIVIDER   12           /* ciclos de CPU por tick de timebase */

/* Vectores de excepción (desplazamiento; base 0 o 0xFFF00000 según MSR[IP]) */
enum {
    EXC_SYSTEM_RESET = 0x0100, EXC_MACHINE_CHECK = 0x0200, EXC_DSI = 0x0300,
    EXC_ISI = 0x0400, EXC_EXTERNAL = 0x0500, EXC_ALIGNMENT = 0x0600,
    EXC_PROGRAM = 0x0700, EXC_FP_UNAVAILABLE = 0x0800, EXC_DECREMENTER = 0x0900,
    EXC_SYSCALL = 0x0C00, EXC_TRACE = 0x0D00, EXC_PERF_MONITOR = 0x0F00,
};

/* Motivo de una excepción de programa, en SRR1 */
#define SRR1_PROGRAM_FP       0x00100000u
#define SRR1_PROGRAM_ILLEGAL  0x00080000u
#define SRR1_PROGRAM_PRIV     0x00040000u
#define SRR1_PROGRAM_TRAP     0x00020000u

typedef struct gekko {
    uint32_t  gpr[32];
    double    fpr[32][2];      /* ps0, ps1 (la FPU llega en la entrega B2) */
    uint32_t  cr;
    uint32_t  xer;
    uint32_t  lr, ctr;
    uint32_t  msr;
    uint32_t  fpscr;
    uint32_t  sr[16];          /* registros de segmento */
    uint32_t  spr[1024];       /* el resto de los SPR, por número */
    uint32_t  pc, npc;

    uint64_t  cycles;          /* instrucciones ejecutadas */
    uint64_t  tb;              /* timebase */
    uint32_t  tb_frac;         /* ciclos hacia el siguiente tick */
    uint32_t  dec;             /* decrementador */
    int       dec_pending;     /* el decrementador pasó por cero */
    int       external;        /* línea de interrupción externa (PI) */

    uint32_t  reserve_addr;    /* lwarx / stwcx. */
    int       reserve;

    uint32_t  illegal;         /* instrucciones no implementadas vistas */
    gc_mem_t *mem;
} gekko_t;

/* Estado de un Gekko recién encendido, sin el IPL: PC = 0xFFF00100, MSR = IP. */
void gekko_reset(gekko_t *c, gc_mem_t *mem);

/* Ejecuta una instrucción (y una excepción pendiente, si hay). */
void gekko_step(gekko_t *c);

/* Ejecuta hasta `cycles` instrucciones. Devuelve las que ejecutó. */
uint64_t gekko_run(gekko_t *c, uint64_t cycles);

/* Línea de interrupción externa (la maneja el PI en la etapa D). */
void gekko_set_external(gekko_t *c, int level);

/* Excepción a pedido: guarda SRR0/SRR1 y salta al vector. srr0 = dirección
 * a la que volverá rfi; srr1_bits = motivo (excepción de programa). */
void gekko_exception(gekko_t *c, uint32_t vector, uint32_t srr0, uint32_t srr1_bits);

/* mfspr / mtspr con sus efectos (DEC, timebase, DMA de la caché bloqueada). */
uint32_t gekko_get_spr(gekko_t *c, uint32_t spr);
void     gekko_set_spr(gekko_t *c, uint32_t spr, uint32_t value);

#endif
