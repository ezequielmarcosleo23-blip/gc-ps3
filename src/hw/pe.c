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
#include "core/log.h"
#include "core/system.h"
#include "hw/pe.h"

#define LOG_MAX 16

static void update_interrupts(gc_pe_t *pe)
{
    gc_pi_t *pi = &pe->sys->pi;
    gc_pi_set_interrupt(pi, PI_INT_PE_TOKEN, pe->signal_token && (pe->ctrl & PE_CTRL_TOKEN_ENABLE));
    gc_pi_set_interrupt(pi, PI_INT_PE_FINISH, pe->signal_finish && (pe->ctrl & PE_CTRL_FINISH_ENABLE));
}

/* SetTokenFinish_OnMainThread */
static void token_finish_event(void *ctx, uint64_t when, uint64_t late)
{
    gc_pe_t *pe = ctx;
    (void)when;
    (void)late;
    pe->event_raised = 0;
    pe->token = pe->token_pending;
    if (pe->token_irq_pending) {
        pe->token_irq_pending = 0;
        pe->signal_token = 1;
        update_interrupts(pe);
    }
    if (pe->finish_irq_pending) {
        pe->finish_irq_pending = 0;
        pe->signal_finish = 1;
        update_interrupts(pe);
    }
}

/* RaiseEvent: uno solo pendiente a la vez */
static void raise_event(gc_pe_t *pe, uint32_t cycles)
{
    if (pe->event_raised)
        return;
    pe->event_raised = 1;
    const uint64_t delay = cycles > GC_PE_MIN_DELAY ? cycles : GC_PE_MIN_DELAY;
    gc_sched_add(&pe->sys->sched, pe->sys->cpu.cycles + delay, "pe", token_finish_event, pe);
}

void gc_pe_set_token(gc_pe_t *pe, uint16_t token, int irq, uint32_t cycles)
{
    pe->token_pending = token;
    pe->token_irq_pending |= irq != 0;
    pe->tokens++;
    raise_event(pe, cycles);
}

void gc_pe_set_finish(gc_pe_t *pe, uint32_t cycles)
{
    pe->finish_irq_pending = 1;
    pe->finishes++;
    raise_event(pe, cycles);
}

/* ---- registros de 16 bits -------------------------------------------------------------- */

static uint16_t *direct_reg(gc_pe_t *pe, uint32_t off)
{
    switch (off) {
    case PE_ZCONF:        return &pe->zconf;
    case PE_ALPHACONF:    return &pe->alphaconf;
    case PE_DSTALPHACONF: return &pe->dstalphaconf;
    case PE_ALPHAMODE:    return &pe->alphamode;
    case PE_ALPHAREAD:    return &pe->alpharead;
    default:              return NULL;
    }
}

static uint16_t pe_read16(gc_pe_t *pe, uint32_t off)
{
    uint16_t *reg = direct_reg(pe, off);
    if (reg)
        return *reg;
    switch (off) {
    case PE_CTRL:  return pe->ctrl;
    case PE_TOKEN: return pe->token;
    default:
        /* Caja envolvente (gx/bbox.c). Contadores de rendimiento: 0, como
         * Dolphin con PerfQueriesEnable apagado (lo normal). */
        if (off >= PE_BBOX_FIRST && off <= PE_BBOX_LAST && !(off & 1u))
            return gc_gx_bbox_get(&pe->sys->gx, (off - PE_BBOX_FIRST) >> 1);
        if (off >= PE_BBOX_FIRST && off <= PE_PERF_LAST)
            return 0;
        if (pe->bad++ < LOG_MAX)
            GC_LOGW("pe: lectura de 0x%02x, que no existe", (unsigned)off);
        return 0;
    }
}

static void pe_write16(gc_pe_t *pe, uint32_t off, uint16_t v)
{
    uint16_t *reg = direct_reg(pe, off);
    if (reg) {
        *reg = v;
        return;
    }
    if (off == PE_CTRL) {
        if (v & PE_CTRL_TOKEN)
            pe->signal_token = 0;
        if (v & PE_CTRL_FINISH)
            pe->signal_finish = 0;
        pe->ctrl = v & (PE_CTRL_TOKEN_ENABLE | PE_CTRL_FINISH_ENABLE);
        update_interrupts(pe);
        return;
    }
    if (pe->bad++ < LOG_MAX)
        GC_LOGW("pe: escritura a 0x%02x ignorada (0x%04x)", (unsigned)off, (unsigned)v);
}

static uint32_t pe_read(void *ctx, uint32_t off, int size)
{
    gc_pe_t *pe = ctx;
    if (size == 4)
        return (uint32_t)pe_read16(pe, off & ~3u) << 16 | pe_read16(pe, (off & ~3u) + 2);
    const uint16_t v = pe_read16(pe, off & ~1u);
    if (size == 2)
        return v;
    return (off & 1) ? (v & 0xFFu) : (uint32_t)(v >> 8);
}

static void pe_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_pe_t *pe = ctx;
    if (size == 4) {
        pe_write16(pe, off & ~3u, (uint16_t)(v >> 16));
        pe_write16(pe, (off & ~3u) + 2, (uint16_t)v);
    } else if (size == 2) {
        pe_write16(pe, off & ~1u, (uint16_t)v);
    } else if (pe->bad++ < LOG_MAX) {
        GC_LOGW("pe: escritura de 8 bits a 0x%02x ignorada", (unsigned)off);
    }
}

void gc_pe_reset(gc_pe_t *pe)
{
    pe->zconf = pe->alphaconf = pe->dstalphaconf = pe->alphamode = pe->alpharead = 0;
    pe->ctrl = 0;
    pe->token = pe->token_pending = 0;
    pe->token_irq_pending = pe->finish_irq_pending = 0;
    pe->event_raised = 0;
    pe->signal_token = pe->signal_finish = 0;
    pe->tokens = pe->finishes = 0;
}

void gc_pe_init(gc_pe_t *pe, struct gc_system *sys)
{
    pe->sys = sys;
    pe->bad = 0;
    gc_pe_reset(pe);
    gc_mmio_register(&sys->mem.io, "PE", GC_MMIO_PE, 0x100, pe_read, pe_write, pe);
}
