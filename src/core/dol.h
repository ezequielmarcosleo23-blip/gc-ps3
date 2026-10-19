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
 * Ejecutables DOL de GameCube: una cabecera de 0x100 bytes (big-endian) con
 * 7 secciones de texto y 11 de datos (posición en el archivo, dirección y
 * tamaño), el BSS y el punto de entrada; después, los datos.
 *
 * Como los cargadores de Nintendo (y Dolphin), cada sección se copia con su
 * tamaño redondeado a 32 bytes si el archivo los tiene, y el BSS no se borra:
 * lo hace el crt0 del programa (en muchos DOL el BSS se solapa con datos).
 */
#ifndef GCPS3_CORE_DOL_H
#define GCPS3_CORE_DOL_H

#include <stddef.h>
#include <stdint.h>

#include "core/mem.h"

#define GC_DOL_TEXT        7
#define GC_DOL_DATA        11
#define GC_DOL_SECTIONS    (GC_DOL_TEXT + GC_DOL_DATA)    /* texto 0-6, datos 7-17 */
#define GC_DOL_HEADER_SIZE 0x100u

typedef struct gc_dol {
    uint32_t offset[GC_DOL_SECTIONS];
    uint32_t addr[GC_DOL_SECTIONS];
    uint32_t size[GC_DOL_SECTIONS];
    uint32_t bss_addr, bss_size;
    uint32_t entry;
} gc_dol_t;

typedef enum gc_dol_error {
    GC_DOL_OK = 0,
    GC_DOL_ERR_SHORT,           /* menos de 0x100 bytes */
    GC_DOL_ERR_FILE,            /* una sección sale del archivo */
    GC_DOL_ERR_RAM,             /* una sección o el BSS salen de la RAM */
    GC_DOL_ERR_OVERLAP,         /* dos secciones se pisan */
    GC_DOL_ERR_ENTRY,           /* la entrada no cae en una sección de texto */
    GC_DOL_ERR_EMPTY,           /* ninguna sección de texto */
    GC_DOL_ERR_WII,             /* escribe HID4: es de Wii */
} gc_dol_error_t;

/* Lee y valida la cabecera. En *bad deja la sección culpable (o -1). */
gc_dol_error_t gc_dol_parse(const uint8_t *data, size_t len, gc_dol_t *dol, int *bad);

const char *gc_dol_strerror(gc_dol_error_t err);

/* Copia las secciones a la RAM. El DOL tiene que venir de gc_dol_parse con
 * los mismos datos. */
void gc_dol_load(const gc_dol_t *dol, const uint8_t *data, size_t len, gc_mem_t *mem);

#endif
