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
 * DVD Interface (0x0C006000) sin disco: la tapa abierta y los comandos que
 * no leen el disco.
 *
 * Como Source/Core/Core/HW/DVD/DVDInterface.cpp de Dolphin (commit cf5cb7ef)
 * cuando no hay disco: DICVR con la tapa abierta, la unidad en "tapa
 * abierta" y cada comando termina a los 300 µs (145 800 ciclos) con TCINT o,
 * si necesita el disco, con DEINT y el error 0x023A00 (sin medio). Inquiry y
 * RequestError contestan como la unidad real. La lectura de ISOs es de una
 * fase posterior.
 */
#ifndef GCPS3_HW_DI_H
#define GCPS3_HW_DI_H

#include <stdint.h>

/* Registros (desplazamiento desde 0x0C006000, de 32 bits) */
enum {
    DI_SR = 0x00, DI_CVR = 0x04, DI_CMDBUF0 = 0x08, DI_CMDBUF1 = 0x0C, DI_CMDBUF2 = 0x10,
    DI_MAR = 0x14, DI_LENGTH = 0x18, DI_CR = 0x1C, DI_IMMBUF = 0x20, DI_CFG = 0x24,
};

/* DISR */
#define DI_SR_BRK        0x01u
#define DI_SR_DEINTMASK  0x02u
#define DI_SR_DEINT      0x04u
#define DI_SR_TCINTMASK  0x08u
#define DI_SR_TCINT      0x10u
#define DI_SR_BRKINTMASK 0x20u
#define DI_SR_BRKINT     0x40u

/* DICVR */
#define DI_CVR_OPEN      0x01u
#define DI_CVR_INTMASK   0x02u
#define DI_CVR_INT       0x04u

#define DI_CR_TSTART     0x01u

#define DI_ERROR_NO_MEDIUM       0x023A00u
#define DI_ERROR_INVALID_COMMAND 0x052000u

struct gc_system;

typedef struct gc_di {
    struct gc_system *sys;
    uint32_t sr, cvr, cmd[3], mar, length, cr, imm, cfg;
    uint32_t state;              /* estado de la unidad (DriveState de Dolphin) */
    uint32_t error;              /* último error */
    int      pending_deint;      /* el comando en curso termina con DEINT */
} gc_di_t;

void gc_di_init(gc_di_t *di, struct gc_system *sys);
void gc_di_reset(gc_di_t *di);

#endif
