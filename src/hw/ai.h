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
 * Audio Interface (0x0C006C00): el contador de muestras del audio que llega
 * del disco (AIS) y su interrupción, y las frecuencias de AIS y AID.
 *
 * Como Source/Core/Core/HW/AudioInterface.cpp de Dolphin (commit cf5cb7ef),
 * sin sonido: con PSTAT el contador avanza una muestra cada 10 116 ciclos
 * (48 kHz; 15 174 a 32 kHz) y AIINT se prende cuando llega a AIIT.
 */
#ifndef GCPS3_HW_AI_H
#define GCPS3_HW_AI_H

#include <stdint.h>

/* Registros (desplazamiento desde 0x0C006C00, de 32 bits) */
enum { AI_CR = 0x00, AI_VR = 0x04, AI_SCNT = 0x08, AI_IT = 0x0C };

/* AICR */
#define AI_CR_PSTAT    0x01u
#define AI_CR_AISFR    0x02u      /* 1: AIS a 48 kHz */
#define AI_CR_AIINTMSK 0x04u
#define AI_CR_AIINT    0x08u
#define AI_CR_AIINTVLD 0x10u
#define AI_CR_SCRESET  0x20u
#define AI_CR_AIDFR    0x40u      /* 1: AID a 32 kHz */

struct gc_system;

typedef struct gc_ai {
    struct gc_system *sys;
    uint32_t control, volume, counter, timing;
    uint64_t last;                /* ciclo hasta el que ya se contaron muestras */
    uint64_t cycles_per_sample;
} gc_ai_t;

void gc_ai_init(gc_ai_t *ai, struct gc_system *sys);
void gc_ai_reset(gc_ai_t *ai);

/* Valor actual del contador de muestras (lo que lee AISCNT). */
uint32_t gc_ai_sample_counter(const gc_ai_t *ai);

#endif
