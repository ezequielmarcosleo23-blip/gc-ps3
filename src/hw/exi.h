/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008-2010 Dolphin Emulator Project
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
 * External Interface (0x0C006800): 3 canales serie con hasta 3 dispositivos
 * cada uno (selección de chip 1, 2 o 4), transferencias inmediatas de hasta 4
 * bytes y por DMA.
 *
 * Como Source/Core/Core/HW/EXI/ de Dolphin (commit cf5cb7ef). Lo único
 * conectado es el chip del IPL (canal 0, CS 2): reloj de tiempo real, SRAM y
 * la UART de depuración. La ROM del IPL no se incluye (D-005): sus lecturas
 * dan 0, sin fuentes. Las ranuras de tarjeta de memoria (canales 0 y 1, CS 1)
 * y los puertos serie están vacíos.
 *
 * La SRAM arranca con los valores de Dolphin (Sram.cpp): inglés, estéreo,
 * configuración inicial hecha y sus sumas de control. El reloj cuenta en
 * segundos desde el 2000-01-01: el de gc_boot_config_t más el tiempo emulado.
 */
#ifndef GCPS3_HW_EXI_H
#define GCPS3_HW_EXI_H

#include <stddef.h>
#include <stdint.h>

#define GC_EXI_CHANNELS 3
#define GC_SRAM_SIZE    0x44

/* Registros de cada canal (desplazamiento desde 0x0C006800 + 0x14 * canal) */
enum { EXI_CSR = 0x00, EXI_MAR = 0x04, EXI_LENGTH = 0x08, EXI_CR = 0x0C, EXI_DATA = 0x10 };

/* CSR */
#define EXI_CSR_EXIINTMASK 0x0001u
#define EXI_CSR_EXIINT     0x0002u
#define EXI_CSR_TCINTMASK  0x0004u
#define EXI_CSR_TCINT      0x0008u
#define EXI_CSR_CLK_MASK   0x0070u
#define EXI_CSR_CS_SHIFT   7
#define EXI_CSR_CS_MASK    0x0380u
#define EXI_CSR_EXTINTMASK 0x0400u
#define EXI_CSR_EXTINT     0x0800u
#define EXI_CSR_EXT        0x1000u
#define EXI_CSR_ROMDIS     0x2000u

/* CR */
#define EXI_CR_TSTART 0x01u
#define EXI_CR_DMA    0x02u

/* Comandos del chip del IPL (dirección << 6; bit 31 = escritura) */
#define EXI_IPL_RTC   0x20000000u
#define EXI_IPL_SRAM  0x20000100u
#define EXI_IPL_UART  0x20010000u

typedef struct gc_exi_channel {
    uint32_t csr, mar, length, cr, data;
} gc_exi_channel_t;

struct gc_system;

typedef struct gc_exi {
    struct gc_system *sys;
    gc_exi_channel_t ch[GC_EXI_CHANNELS];
    /* chip del IPL */
    uint32_t ipl_cmd;
    uint32_t ipl_cmd_bytes;
    uint32_t ipl_cursor;
    uint8_t  sram[GC_SRAM_SIZE];
} gc_exi_t;

void gc_exi_init(gc_exi_t *exi, struct gc_system *sys);
void gc_exi_reset(gc_exi_t *exi);

/* Suma de control de la SRAM (bytes 0x10-0x17), como FixSRAMChecksums. */
void gc_sram_fix_checksums(uint8_t sram[GC_SRAM_SIZE]);

/* Segundos desde el 2000-01-01 que da el reloj ahora. */
uint32_t gc_exi_rtc(const gc_exi_t *exi);

#endif
