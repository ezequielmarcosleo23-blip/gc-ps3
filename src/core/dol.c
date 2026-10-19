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
#include <string.h>

#include "core/dol.h"
#include "core/log.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t align32(uint64_t v)
{
    return (v + 31u) & ~(uint64_t)31u;
}

/* El tramo [addr, addr + size) está en los 24 MB de RAM. */
static int in_ram(uint32_t addr, uint32_t size)
{
    if (gc_mem_region(addr) != GC_REGION_RAM)
        return 0;
    const uint32_t phys = gc_mem_phys(addr);
    return phys < GC_RAM_SIZE && size <= GC_RAM_SIZE - phys;
}

/* mtspr HID4, rS: solo existe en Broadway (Wii). Mismo criterio que Dolphin. */
static int writes_hid4(const uint8_t *p, uint32_t size)
{
    for (uint32_t k = 0; k + 4 <= size; k += 4)
        if ((be32(p + k) & 0xFC1FFFFFu) == 0x7C13FBA6u)
            return 1;
    return 0;
}

gc_dol_error_t gc_dol_parse(const uint8_t *data, size_t len, gc_dol_t *dol, int *bad)
{
    int dummy;
    if (!bad)
        bad = &dummy;
    *bad = -1;
    memset(dol, 0, sizeof *dol);
    if (len < GC_DOL_HEADER_SIZE)
        return GC_DOL_ERR_SHORT;

    for (int s = 0; s < GC_DOL_SECTIONS; s++) {
        dol->offset[s] = be32(data + 0x00 + 4 * s);
        dol->addr[s] = be32(data + 0x48 + 4 * s);
        dol->size[s] = be32(data + 0x90 + 4 * s);
    }
    dol->bss_addr = be32(data + 0xD8);
    dol->bss_size = be32(data + 0xDC);
    dol->entry = be32(data + 0xE0);

    int texts = 0;
    for (int s = 0; s < GC_DOL_SECTIONS; s++) {
        if (dol->size[s] == 0)
            continue;
        *bad = s;
        if ((uint64_t)dol->offset[s] + dol->size[s] > len)
            return GC_DOL_ERR_FILE;
        if (!in_ram(dol->addr[s], dol->size[s]))
            return GC_DOL_ERR_RAM;
        const uint32_t p0 = gc_mem_phys(dol->addr[s]), p1 = p0 + dol->size[s];
        for (int t = 0; t < s; t++) {
            if (dol->size[t] == 0)
                continue;
            const uint32_t q0 = gc_mem_phys(dol->addr[t]), q1 = q0 + dol->size[t];
            if (p0 < q1 && q0 < p1)
                return GC_DOL_ERR_OVERLAP;
        }
        if (s < GC_DOL_TEXT) {
            texts++;
            if (writes_hid4(data + dol->offset[s], dol->size[s]))
                return GC_DOL_ERR_WII;
        }
    }
    *bad = -1;
    if (!texts)
        return GC_DOL_ERR_EMPTY;
    if (dol->bss_size && !in_ram(dol->bss_addr, dol->bss_size))
        return GC_DOL_ERR_RAM;

    for (int s = 0; s < GC_DOL_TEXT; s++) {
        const uint32_t p0 = gc_mem_phys(dol->addr[s]), e = gc_mem_phys(dol->entry);
        if (dol->size[s] && gc_mem_region(dol->entry) == GC_REGION_RAM && e >= p0 && e - p0 < dol->size[s])
            return GC_DOL_OK;
    }
    return GC_DOL_ERR_ENTRY;
}

const char *gc_dol_strerror(gc_dol_error_t err)
{
    switch (err) {
    case GC_DOL_OK:          return "ok";
    case GC_DOL_ERR_SHORT:   return "archivo mas chico que la cabecera de un DOL";
    case GC_DOL_ERR_FILE:    return "una seccion sale del archivo";
    case GC_DOL_ERR_RAM:     return "una seccion o el BSS salen de los 24 MB de RAM";
    case GC_DOL_ERR_OVERLAP: return "dos secciones se pisan";
    case GC_DOL_ERR_ENTRY:   return "la entrada no esta en una seccion de texto";
    case GC_DOL_ERR_EMPTY:   return "no tiene secciones de texto";
    case GC_DOL_ERR_WII:     return "es un DOL de Wii (escribe HID4)";
    }
    return "error desconocido";
}

void gc_dol_load(const gc_dol_t *dol, const uint8_t *data, size_t len, gc_mem_t *mem)
{
    for (int s = 0; s < GC_DOL_SECTIONS; s++) {
        if (dol->size[s] == 0)
            continue;
        /* Como el IPL y Dolphin: el tamaño redondeado a 32 bytes, si el archivo
         * y la RAM alcanzan; si no, el tamaño exacto. En orden de sección. */
        uint32_t n = dol->size[s];
        const uint64_t a = align32(n);
        if ((uint64_t)dol->offset[s] + a <= len && in_ram(dol->addr[s], (uint32_t)a))
            n = (uint32_t)a;
        else if (a != n)
            GC_LOGW("dol: la seccion %d no llega a un multiplo de 32 bytes", s);
        memcpy(gc_ram_ptr(mem, dol->addr[s], n), data + dol->offset[s], n);
    }
}
