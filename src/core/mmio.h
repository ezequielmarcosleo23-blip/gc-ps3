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
 * Registros de hardware de la GameCube (dirección física 0x0C000000, virtual
 * 0xCC000000). Cada bloque (CP, PE, VI...) registra un manejador; la tabla va
 * por páginas de 256 bytes, así que encontrar el manejador es un índice.
 *
 * Los manejadores reciben el desplazamiento dentro de su bloque y el tamaño del
 * acceso (1, 2 o 4 bytes). Un acceso sin manejador devuelve 0 (las escrituras
 * se pierden), se cuenta y se registra en el log las primeras veces.
 */
#ifndef GCPS3_CORE_MMIO_H
#define GCPS3_CORE_MMIO_H

#include <stdint.h>

#define GC_MMIO_PHYS     0x0C000000u
#define GC_MMIO_SPAN     0x00010000u     /* 0x0C000000-0x0C00FFFF */
#define GC_MMIO_PAGE     8               /* páginas de 256 bytes */
#define GC_MMIO_PAGES    (GC_MMIO_SPAN >> GC_MMIO_PAGE)
#define GC_MMIO_LOG_MAX  16              /* accesos sin manejador que se registran */

/* Bloques y su desplazamiento desde 0x0C000000. */
enum {
    GC_MMIO_CP   = 0x0000,   /* Command Processor */
    GC_MMIO_PE   = 0x1000,   /* Pixel Engine */
    GC_MMIO_VI   = 0x2000,   /* Video Interface */
    GC_MMIO_PI   = 0x3000,   /* Processor Interface (interrupciones) */
    GC_MMIO_MI   = 0x4000,   /* Memory Interface */
    GC_MMIO_DSP  = 0x5000,   /* DSP y ARAM */
    GC_MMIO_DI   = 0x6000,   /* lector de disco */
    GC_MMIO_SI   = 0x6400,   /* mandos */
    GC_MMIO_EXI  = 0x6800,   /* tarjetas de memoria, RTC, IPL */
    GC_MMIO_AI   = 0x6C00,   /* audio */
    GC_MMIO_GXFIFO = 0x8000, /* FIFO de GX (write gather pipe) */
};

typedef uint32_t (*gc_mmio_read_fn)(void *ctx, uint32_t offset, int size);
typedef void (*gc_mmio_write_fn)(void *ctx, uint32_t offset, uint32_t value, int size);

typedef struct gc_mmio_block {
    const char       *name;
    uint32_t          base;     /* desplazamiento desde 0x0C000000 */
    uint32_t          size;
    gc_mmio_read_fn   read;     /* NULL: el bloque no se lee */
    gc_mmio_write_fn  write;    /* NULL: el bloque no se escribe */
    void             *ctx;
} gc_mmio_block_t;

#define GC_MMIO_MAX_BLOCKS 16

typedef struct gc_mmio {
    gc_mmio_block_t  blocks[GC_MMIO_MAX_BLOCKS];
    int              nblocks;
    int8_t           page[GC_MMIO_PAGES];   /* índice del bloque por página; -1 = libre */
    uint32_t         unmapped;              /* accesos sin manejador */
} gc_mmio_t;

void gc_mmio_init(gc_mmio_t *io);

/* base y size alineados a 256 bytes, dentro de 0x0C000000-0x0C00FFFF y sin
 * pisar otro bloque. Devuelve 0, o -1 si no se pudo registrar. */
int gc_mmio_register(gc_mmio_t *io, const char *name, uint32_t base, uint32_t size,
                     gc_mmio_read_fn read, gc_mmio_write_fn write, void *ctx);

/* phys: dirección física (0x0C00xxxx). size: 1, 2 o 4. */
uint32_t gc_mmio_read(gc_mmio_t *io, uint32_t phys, int size);
void     gc_mmio_write(gc_mmio_t *io, uint32_t phys, uint32_t value, int size);

#endif
