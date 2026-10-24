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
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "hw/cp.h"

#define LOG_MAX 16

/* ---- interrupción y estado ---------------------------------------------------------- */

/* UpdateInterrupts: con un solo hilo, en el acto. */
static void update_interrupts(gc_cp_t *cp, int set)
{
    cp->irq_set = set;
    gc_pi_set_interrupt(&cp->sys->pi, PI_INT_CP, set);
    gc_gx_run_gpu(&cp->sys->gx);
}

/* Las marcas y la interrupción (lo común de SetCPStatusFromGPU y FromCPU). */
static void check_interrupt(gc_cp_t *cp)
{
    gc_cp_fifo_t *f = &cp->fifo;
    f->hi = f->distance > f->hi_wm;
    f->lo = f->distance < f->lo_wm;

    const int bp_int = f->breakpoint && f->bp_int;
    const int ovf_int = f->hi && f->hi_int;
    const int undf_int = f->lo && f->lo_int;
    const int irq = (bp_int || ovf_int || undf_int) && (cp->ctrl & CP_CR_READ);
    if (irq != cp->irq_set)
        update_interrupts(cp, irq);
}

void gc_cp_status_from_gpu(gc_cp_t *cp)
{
    gc_cp_fifo_t *f = &cp->fifo;
    if (f->bp_enable) {
        if (f->bp == f->rptr)
            f->breakpoint = 1;
        else
            f->breakpoint = 0;
    } else {
        f->breakpoint = 0;
    }
    check_interrupt(cp);
}

/* SetCPStatusFromCPU: igual, sin tocar el breakpoint. */
static void status_from_cpu(gc_cp_t *cp)
{
    check_interrupt(cp);
}

/* SetCpStatusRegister: lo que se lee en SR. */
static void update_status_register(gc_cp_t *cp)
{
    const gc_cp_fifo_t *f = &cp->fifo;
    uint16_t sr = cp->status & ~(uint16_t)(CP_SR_OVERFLOW | CP_SR_UNDERFLOW | CP_SR_READ_IDLE |
                                           CP_SR_CMD_IDLE | CP_SR_BREAKPOINT);
    if (f->breakpoint)
        sr |= CP_SR_BREAKPOINT;
    if (!f->distance || f->rptr == f->wptr)
        sr |= CP_SR_READ_IDLE;
    if (!f->distance || gc_cp_at_breakpoint(cp) || !f->read_enable)
        sr |= CP_SR_CMD_IDLE;
    if (f->lo)
        sr |= CP_SR_UNDERFLOW;
    if (f->hi)
        sr |= CP_SR_OVERFLOW;
    cp->status = sr;
}

/* SetCpControlRegister */
static void update_control(gc_cp_t *cp)
{
    gc_cp_fifo_t *f = &cp->fifo;
    /* Antes de dejar de leer, la GPU se pone al día */
    if (f->read_enable && !(cp->ctrl & CP_CR_READ))
        gc_gx_sync_for_register_access(&cp->sys->gx);
    f->read_enable = (cp->ctrl & CP_CR_READ) != 0;
    f->bp_int = (cp->ctrl & CP_CR_BP_INT) != 0;
    f->bp_enable = (cp->ctrl & CP_CR_BP_ENABLE) != 0;
    f->hi_int = (cp->ctrl & CP_CR_OVF_INT) != 0;
    f->lo_int = (cp->ctrl & CP_CR_UNDF_INT) != 0;
    f->link = (cp->ctrl & CP_CR_LINK) != 0;
}

void gc_cp_gather_pipe_bursted(gc_cp_t *cp)
{
    gc_cp_fifo_t *f = &cp->fifo;
    gc_pi_t *pi = &cp->sys->pi;

    status_from_cpu(cp);
    /* Sin enlace, lo escrito no es de este FIFO */
    if (!(cp->ctrl & CP_CR_LINK)) {
        gc_gx_run_gpu(&cp->sys->gx);
        return;
    }
    if (f->wptr == f->end)
        f->wptr = f->base;
    else
        f->wptr += 32;
    if ((cp->ctrl & CP_CR_READ) && (cp->ctrl & CP_CR_LINK)) {
        pi->fifo_wptr = f->wptr;
        pi->fifo_base = f->base;
        pi->fifo_end = f->end;
    }
    f->distance += 32;
    gc_gx_run_gpu(&cp->sys->gx);
}

void gc_cp_reset_fifo(gc_cp_t *cp)
{
    cp->ctrl = CP_CR_LINK;
    update_control(cp);
    cp->perf_select = 0;
    cp->unk_0a = 0;
    cp->fifo.lo_wm = 0;
    cp->fifo.hi_wm = GC_CP_ADDR_MASK & ~31u;
    update_status_register(cp);
}

/* ---- registros de 16 bits -------------------------------------------------------------- */

static uint16_t lo16(uint32_t v) { return (uint16_t)v; }
static uint16_t hi16(uint32_t v) { return (uint16_t)(v >> 16); }

static void set_lo(uint32_t *reg, uint16_t v)
{
    *reg = (*reg & 0xFFFF0000u) | (v & GC_CP_LO_MASK);
}

static void set_hi(uint32_t *reg, uint16_t v)
{
    *reg = (*reg & 0x0000FFFFu) | ((uint32_t)(v & GC_CP_HI_MASK) << 16);
}

/* Los pares que se leen y escriben directo; NULL si no es uno de ellos. */
static uint32_t *direct_reg(gc_cp_t *cp, uint32_t off)
{
    gc_cp_fifo_t *f = &cp->fifo;
    switch (off & ~2u) {
    case CP_FIFO_BASE_LO:  return &f->base;
    case CP_FIFO_END_LO:   return &f->end;
    case CP_FIFO_HI_WM_LO: return &f->hi_wm;
    case CP_FIFO_LO_WM_LO: return &f->lo_wm;
    case CP_FIFO_WPTR_LO:  return &f->wptr;
    case CP_FIFO_BP_LO:    return &f->bp;
    default:               return NULL;
    }
}

static void bad_access(gc_cp_t *cp, const char *what, uint32_t off)
{
    if (cp->bad++ < LOG_MAX)
        GC_LOGW("cp: %s del registro 0x%02x, que no existe o no se %s", what, (unsigned)off,
                what[0] == 'l' ? "lee" : "escribe");
}

static uint16_t cp_read16(gc_cp_t *cp, uint32_t off)
{
    gc_cp_fifo_t *f = &cp->fifo;
    uint32_t *reg = direct_reg(cp, off);
    if (reg)
        return (off & 2) ? hi16(*reg) : lo16(*reg);

    switch (off) {
    case CP_SR:
        gc_gx_sync_for_register_access(&cp->sys->gx);
        update_status_register(cp);
        return cp->status;
    case CP_CR:          return cp->ctrl;
    case CP_CLEAR:       return 0;
    case CP_PERF_SELECT: return cp->perf_select;
    case CP_UNK_0A:      return cp->unk_0a;
    case CP_FIFO_DIST_LO:
        return lo16(f->distance);
    case CP_FIFO_DIST_HI:
        gc_gx_sync_for_register_access(&cp->sys->gx);
        return hi16(f->distance);
    case CP_FIFO_RPTR_LO:
        return lo16(f->rptr);
    case CP_FIFO_RPTR_HI:
        gc_gx_sync_for_register_access(&cp->sys->gx);
        return hi16(f->rptr);
    case CP_CLKS_PER_VTX_OUT:
        return 4;
    default:
        if (off >= CP_METRICS_FIRST && off <= CP_METRICS_LAST)
            return 0;                                   /* métricas: fijas en 0 */
        bad_access(cp, "lectura", off);
        return 0;
    }
}

static void cp_write16(gc_cp_t *cp, uint32_t off, uint16_t v)
{
    gc_cp_fifo_t *f = &cp->fifo;
    uint32_t *reg = direct_reg(cp, off);
    if (reg) {
        if (off & 2)
            set_hi(reg, v);
        else
            set_lo(reg, v);
        return;
    }

    switch (off) {
    case CP_CR:
        cp->ctrl = v & 0x3Fu;
        update_control(cp);
        gc_gx_run_gpu(&cp->sys->gx);
        return;
    case CP_CLEAR:
        cp->clear = v;                                  /* Dolphin no lo emula */
        gc_gx_run_gpu(&cp->sys->gx);
        return;
    case CP_PERF_SELECT:
        cp->perf_select = v & 0x0007u;
        return;
    case CP_UNK_0A:
        cp->unk_0a = v & 0x00FFu;
        return;
    case CP_FIFO_DIST_LO:
        set_lo(&f->distance, v);
        return;
    case CP_FIFO_DIST_HI:
        gc_gx_sync_for_register_access(&cp->sys->gx);
        set_hi(&f->distance, v);
        gc_gx_run_gpu(&cp->sys->gx);
        return;
    case CP_FIFO_RPTR_LO:
        set_lo(&f->rptr, v);
        return;
    case CP_FIFO_RPTR_HI:
        gc_gx_sync_for_register_access(&cp->sys->gx);
        set_hi(&f->rptr, v);
        return;
    default:
        bad_access(cp, "escritura", off);
        return;
    }
}

/* Los de 32 bits son dos de 16, la mitad de la dirección menor arriba y primero
 * (MMIO::ReadToSmaller y WriteToSmaller de Dolphin). */
static uint32_t cp_read(void *ctx, uint32_t off, int size)
{
    gc_cp_t *cp = ctx;
    if (size == 4)
        return (uint32_t)cp_read16(cp, off & ~3u) << 16 | cp_read16(cp, (off & ~3u) + 2);
    const uint16_t v = cp_read16(cp, off & ~1u);
    if (size == 2)
        return v;
    return (off & 1) ? (v & 0xFFu) : (uint32_t)(v >> 8);
}

static void cp_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_cp_t *cp = ctx;
    if (size == 4) {
        cp_write16(cp, off & ~3u, (uint16_t)(v >> 16));
        cp_write16(cp, (off & ~3u) + 2, (uint16_t)v);
    } else if (size == 2) {
        cp_write16(cp, off & ~1u, (uint16_t)v);
    } else {
        bad_access(cp, "escritura de 8 bits", off);
    }
}

void gc_cp_reset(gc_cp_t *cp)
{
    memset(&cp->fifo, 0, sizeof cp->fifo);
    cp->status = CP_SR_CMD_IDLE | CP_SR_READ_IDLE;
    cp->ctrl = 0;
    cp->clear = 0;
    cp->perf_select = 0;
    cp->unk_0a = 0;
    cp->irq_set = 0;
}

void gc_cp_init(gc_cp_t *cp, struct gc_system *sys)
{
    cp->sys = sys;
    cp->bad = 0;
    gc_cp_reset(cp);
    gc_mmio_register(&sys->mem.io, "CP", GC_MMIO_CP, 0x100, cp_read, cp_write, cp);
}
