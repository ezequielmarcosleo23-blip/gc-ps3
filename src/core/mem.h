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
 * Memoria emulada de la GameCube y su mapa de direcciones (docs/mapa-memoria.md).
 *
 *   0x80000000-0x817FFFFF  RAM principal, 24 MB, con caché     ┐ mismo búfer;
 *   0xC0000000-0xC17FFFFF  la misma RAM, sin caché             │ también las
 *   0x00000000-0x017FFFFF  física (traducción apagada)         ┘ físicas
 *   0xCC000000-0xCC00FFFF  registros de hardware (core/mmio.h)
 *   0xE0000000-0xE0003FFF  caché bloqueada, 16 KB
 *   ARAM (16 MB)           sin dirección: la usa el DMA del DSP
 *
 * La RAM vive en un búfer de 32 MB y se direcciona con la máscara 0x01FFFFFF
 * (D-008): una dirección de RAM fuera de los 24 MB cae en la cola del búfer y
 * nunca falla. Lo que no es RAM, registros ni caché bloqueada (por ejemplo el
 * acceso directo al EFB en 0xC8000000) lee 0, ignora la escritura y se cuenta.
 *
 * Gekko es big-endian como el PPU: en la PS3 los accesos son copias directas
 * y en la PC se invierten bytes (core/endian.h). Acepta direcciones no
 * alineadas en la RAM, como el hardware.
 */
#ifndef GCPS3_CORE_MEM_H
#define GCPS3_CORE_MEM_H

#include <stddef.h>
#include <stdint.h>

#include "core/mmio.h"

#define GC_RAM_SIZE     0x01800000u   /* 24 MB que ve el juego */
#define GC_RAM_ALLOC    0x02000000u   /* 32 MB reservados */
#define GC_RAM_MASK     0x01FFFFFFu
#define GC_ARAM_SIZE    0x01000000u   /* 16 MB */
#define GC_LCACHE_BASE  0xE0000000u
#define GC_LCACHE_SIZE  0x00004000u   /* 16 KB */
#define GC_LOG_BAD_MAX  16            /* accesos inválidos que se registran */

typedef enum gc_region {
    GC_REGION_NONE = 0,
    GC_REGION_RAM,
    GC_REGION_MMIO,
    GC_REGION_LCACHE,
} gc_region_t;

typedef struct gc_mem {
    uint8_t   *ram;        /* GC_RAM_ALLOC bytes, alineado a 64 KB */
    uint8_t   *aram;       /* GC_ARAM_SIZE bytes */
    uint8_t   *lcache;     /* GC_LCACHE_SIZE bytes */
    gc_mmio_t  io;
    uint32_t   bad;        /* accesos a direcciones sin nada */
    void      *raw[3];     /* lo que devolvió malloc, para liberar */
} gc_mem_t;

/* Reserva y pone a cero todo; registra cero bloques MMIO. 0 o -1 sin memoria. */
int  gc_mem_init(gc_mem_t *m);
void gc_mem_free(gc_mem_t *m);

/* Qué hay en una dirección efectiva (con el mapa por omisión de los BAT). */
gc_region_t gc_mem_region(uint32_t addr);

/* Dirección física de una efectiva de los segmentos 0x0, 0x8 y 0xC. */
static inline uint32_t gc_mem_phys(uint32_t addr)
{
    return addr & 0x3FFFFFFFu;
}

uint8_t  gc_read8(gc_mem_t *m, uint32_t addr);
uint16_t gc_read16(gc_mem_t *m, uint32_t addr);
uint32_t gc_read32(gc_mem_t *m, uint32_t addr);
uint64_t gc_read64(gc_mem_t *m, uint32_t addr);
void     gc_write8(gc_mem_t *m, uint32_t addr, uint8_t v);
void     gc_write16(gc_mem_t *m, uint32_t addr, uint16_t v);
void     gc_write32(gc_mem_t *m, uint32_t addr, uint32_t v);
void     gc_write64(gc_mem_t *m, uint32_t addr, uint64_t v);

/* Puntero a len bytes de RAM desde addr (efectiva o física), sin pasar de los
 * 24 MB reales; NULL si el tramo no es RAM. Para cargadores y DMA. */
uint8_t *gc_ram_ptr(gc_mem_t *m, uint32_t addr, uint32_t len);

/* Como lo ve el hardware de video (y Dolphin, Memmap::GetPointerForRange):
 * los 2 bits de arriba no cuentan y el tramo tiene que caber en los 24 MB.
 * NULL si no. Para el FIFO de GX, las display lists y los arreglos. */
uint8_t *gc_ram_phys(gc_mem_t *m, uint32_t addr, uint32_t len);

#endif
