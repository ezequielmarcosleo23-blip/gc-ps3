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
 * La GameCube armada: memoria, CPU, planificador, el hardware de src/hw y el
 * arranque por HLE.
 *
 * gc_system_boot_dol deja la máquina como la deja el IPL antes de saltar a
 * un ejecutable, con los valores que usa Dolphin para arrancar un DOL
 * (D-032, ver docs/arranque-hle.md); el IPL de Nintendo no se usa (D-005).
 */
#ifndef GCPS3_CORE_SYSTEM_H
#define GCPS3_CORE_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

#include "core/dol.h"
#include "core/hle.h"
#include "core/mem.h"
#include "core/sched.h"
#include "cpu/gekko.h"
#include "gx/gx.h"
#include "hw/ai.h"
#include "hw/cp.h"
#include "hw/di.h"
#include "hw/dsp.h"
#include "hw/exi.h"
#include "hw/gpfifo.h"
#include "hw/mi.h"
#include "hw/pe.h"
#include "hw/pi.h"
#include "hw/si.h"
#include "hw/vi.h"

#define GC_CPU_CLOCK   486000000u      /* Hz */
#define GC_BUS_CLOCK   162000000u
#define GC_TB_CLOCK    (GC_CPU_CLOCK / GEKKO_TB_DIVIDER)    /* 40,5 MHz */
#define GC_BOOT_CYCLES 20000u                               /* ciclo de la primera instrucción del DOL */
#define GC_EPOCH_UNIX  946684800ull    /* 2000-01-01 00:00:00 UTC, en tiempo Unix */

typedef struct gc_boot_config {
    int      pal;                      /* modo de video que dejó el IPL: 0 NTSC, 1 PAL */
    uint64_t rtc;                      /* segundos desde el 2000-01-01 */
} gc_boot_config_t;

typedef struct gc_system {
    gc_mem_t   mem;
    gekko_t    cpu;
    gc_sched_t sched;
    gc_hle_t   hle;
    gc_pi_t    pi;
    gc_vi_t    vi;
    gc_si_t    si;
    gc_exi_t   exi;
    gc_mi_t    mi;
    gc_ai_t    ai;
    gc_dsp_t   dsp;
    gc_di_t    di;
    gc_gpfifo_t gpfifo;                /* write-gather pipe (0x0C008000) */
    gc_cp_t    cp;                     /* Command Processor: registros y FIFO de GX */
    gc_pe_t    pe;                     /* Pixel Engine: token y "draw done" */
    gc_gx_t    gx;                     /* la GPU: lector de comandos y estado de BP, CP y XF */
    uint64_t   rtc_boot;               /* segundos desde 2000 al arrancar (reloj del IPL) */
} gc_system_t;

/* Reserva la memoria y deja la CPU recién encendida. 0, o -1 sin memoria. */
int  gc_system_init(gc_system_t *s);
void gc_system_free(gc_system_t *s);

/* RAM en cero, estado de CPU y memoria baja del IPL, PC = entry. */
void gc_system_hle_boot(gc_system_t *s, const gc_boot_config_t *cfg, uint32_t entry);

/* Valida y carga el DOL y arranca en su entrada. Si falla, no cambia nada. */
gc_dol_error_t gc_system_boot_dol(gc_system_t *s, const uint8_t *data, size_t len,
                                  const gc_boot_config_t *cfg);

/* Engancha las funciones conocidas de un .map (después de cargar el DOL). */
int gc_system_load_map(gc_system_t *s, const char *text, size_t len);

/* Corre hasta `cycles` ciclos de CPU atendiendo los eventos a tiempo; vuelve
 * antes si se prende cpu.stop. Devuelve los ciclos corridos. */
uint64_t gc_system_run(gc_system_t *s, uint64_t cycles);

#endif
