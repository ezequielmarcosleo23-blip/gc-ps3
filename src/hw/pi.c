/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008-2009 Dolphin Emulator Project
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
#include "core/log.h"
#include "core/system.h"
#include "hw/pi.h"

#define GX_ADDR_MASK 0x03FFFFE0u    /* direcciones físicas del FIFO de GX, alineadas a 32 */

static void update(gc_pi_t *pi)
{
    gekko_set_external(&pi->sys->cpu, (pi->cause & pi->mask) != 0);
}

void gc_pi_set_interrupt(gc_pi_t *pi, uint32_t cause, int set)
{
    if (set)
        pi->cause |= cause;
    else
        pi->cause &= ~cause;
    update(pi);
}

void gc_pi_reset(gc_pi_t *pi)
{
    pi->cause = PI_RSW_STATE | PI_INT_VI;   /* como Dolphin al encender */
    pi->mask = 0;
    pi->fifo_base = pi->fifo_end = pi->fifo_wptr = 0;
    pi->error_cause = pi->error_addr = 0;
    pi->reset_code = 0;                     /* arranque en frío */
    pi->unknown = 0x000001FFu;
    pi->bus_strength = 0x02492492u;
    update(pi);
}

static uint32_t read32(gc_pi_t *pi, uint32_t off)
{
    switch (off) {
    case PI_INTSR:        return pi->cause;
    case PI_INTMR:        return pi->mask;
    case PI_FIFO_BASE:    return pi->fifo_base;
    case PI_FIFO_END:     return pi->fifo_end;
    case PI_FIFO_WPTR:    return pi->fifo_wptr;
    case PI_ERROR_CAUSE:  return pi->error_cause;
    case PI_ERROR_ADDR:   return pi->error_addr;
    case PI_RESET_CODE:   return pi->reset_code;
    case PI_UNKNOWN:      return pi->unknown;
    case PI_FLIPPER_REV:  return PI_FLIPPER_REV_C;
    case PI_BUS_STRENGTH: return pi->bus_strength;
    default:              return 0;
    }
}

static void write32(gc_pi_t *pi, uint32_t off, uint32_t v)
{
    switch (off) {
    case PI_INTSR:        pi->cause &= ~v; update(pi); break;    /* 1 borra */
    case PI_INTMR:        pi->mask = v; update(pi); break;
    case PI_FIFO_BASE:    pi->fifo_base = v & GX_ADDR_MASK; break;
    case PI_FIFO_END:     pi->fifo_end = v & GX_ADDR_MASK; break;
    case PI_FIFO_WPTR:    pi->fifo_wptr = v & GX_ADDR_MASK; break;
    case PI_FIFO_RESET:   break;                                  /* GX: después de la Fase 1 */
    case PI_ERROR_CAUSE:  pi->error_cause = v & 7u; break;
    case PI_RESET_CODE:   pi->reset_code = v; break;
    case PI_UNKNOWN:      pi->unknown = v & 0x3FFu; break;
    case PI_BUS_STRENGTH: pi->bus_strength = v & 0x07FFFFFFu; break;
    default:
        GC_LOGW("pi: escritura a 0x%02x sin implementar (0x%08x)", (unsigned)off, (unsigned)v);
        break;
    }
}

/* Los registros son de 32 bits; las lecturas de 16 toman la mitad (Dolphin).
 * Las escrituras de 8 o 16 bits no existen en el hardware. */
static uint32_t pi_read(void *ctx, uint32_t off, int size)
{
    const uint32_t v = read32(ctx, off & ~3u);
    if (size == 4)
        return v;
    if (size == 2)
        return (off & 2) ? (v & 0xFFFFu) : (v >> 16);
    return (v >> (24 - 8 * (off & 3))) & 0xFFu;
}

static void pi_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    if (size != 4) {
        GC_LOGW("pi: escritura de %d bits a 0x%02x ignorada", size * 8, (unsigned)off);
        return;
    }
    write32(ctx, off, v);
}

void gc_pi_init(gc_pi_t *pi, struct gc_system *sys)
{
    pi->sys = sys;
    gc_pi_reset(pi);
    gc_mmio_register(&sys->mem.io, "PI", GC_MMIO_PI, 0x100, pi_read, pi_write, pi);
}
