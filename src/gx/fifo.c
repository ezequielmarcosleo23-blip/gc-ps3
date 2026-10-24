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
 * La GPU corriendo en el hilo de la CPU: VideoCommon/Fifo.cpp de Dolphin
 * (RunGpu, SyncGPUCallback, RunGpuOnCpu, SyncGPUForRegisterAccess,
 * ReadDataFromFifo) con un solo hilo y sin el hilo determinista.
 */
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/gx.h"

#define LOG_MAX 16

/* ---- búfer de video ----------------------------------------------------------------- */

void gc_gx_reset_video_buffer(gc_gx_t *gx)
{
    gx->vrd = gx->vwr = 0;
}

/* ReadDataFromFifo: 32 bytes de la RAM al búfer, corriendo lo que quedaba al
 * principio si no entra. */
static void read_block(gc_gx_t *gx, uint32_t addr)
{
    if (GC_GPFIFO_BLOCK > GC_GX_VBUF_SIZE - gx->vwr) {
        const uint32_t existing = gx->vwr - gx->vrd;
        if (GC_GPFIFO_BLOCK > GC_GX_VBUF_SIZE - existing) {
            if (gx->warnings++ < LOG_MAX)
                GC_LOGW("gx: el bufer de video se lleno (%u bytes de un comando sin terminar, "
                        "bloque en 0x%08x)", (unsigned)existing, (unsigned)addr);
            return;
        }
        memmove(gx->vbuf, gx->vbuf + gx->vrd, existing);
        gx->vwr = existing;
        gx->vrd = 0;
    }
    const uint8_t *src = gc_ram_phys(&gx->sys->mem, addr, GC_GPFIFO_BLOCK);
    if (src)
        memcpy(gx->vbuf + gx->vwr, src, GC_GPFIFO_BLOCK);
    else if (gx->warnings++ < LOG_MAX)
        GC_LOGW("gx: bloque del FIFO en 0x%08x fuera de la RAM", (unsigned)addr);
    /* Dolphin avanza aunque la copia falle (CopyFromEmu solo avisa) */
    gx->vwr += GC_GPFIFO_BLOCK;
}

/* ---- tiempo -------------------------------------------------------------------------- */

int32_t gc_gx_run_gpu_on_cpu(gc_gx_t *gx, int32_t ticks)
{
    gc_cp_t *cp = &gx->sys->cp;
    gc_cp_fifo_t *f = &cp->fifo;
    int32_t available = ticks + gx->sync_ticks;

    while (f->read_enable && f->distance && !gc_cp_at_breakpoint(cp) && available >= 0) {
        read_block(gx, f->rptr);
        gx->cycles = 0;
        gx->vrd += gc_gx_run_commands(gx, gx->vbuf + gx->vrd, gx->vwr - gx->vrd);
        available -= (int32_t)gx->cycles;

        if (f->rptr == f->end)
            f->rptr = f->base;
        else
            f->rptr += GC_GPFIFO_BLOCK;
        f->distance -= GC_GPFIFO_BLOCK;
    }

    gc_cp_status_from_gpu(cp);

    /* Lo que sobró no se guarda; la deuda sí */
    gx->sync_ticks = available < 0 ? available : 0;
    if (available >= 0)
        return -1;
    return -available + GC_GX_SLOT;
}

/* SyncGPUCallback: ticks = presupuesto con el que se agendó + atraso */
static void sync_event(void *ctx, uint64_t when, uint64_t late)
{
    gc_gx_t *gx = ctx;
    (void)when;
    const int32_t next = gc_gx_run_gpu_on_cpu(gx, gx->event_ticks + (int32_t)late);
    gx->suspended = next < 0;
    if (!gx->suspended) {
        gx->event_ticks = next;
        gc_sched_add(&gx->sys->sched, gx->sys->cpu.cycles + (uint64_t)next, "gx", sync_event, gx);
    }
}

void gc_gx_run_gpu(gc_gx_t *gx)
{
    if (!gx->suspended)
        return;
    gx->suspended = 0;
    gx->event_ticks = GC_GX_SLOT;
    gc_sched_add(&gx->sys->sched, gx->sys->cpu.cycles + GC_GX_SLOT, "gx", sync_event, gx);
}

void gc_gx_sync_for_register_access(gc_gx_t *gx)
{
    gc_gx_run_gpu_on_cpu(gx, GC_GX_SLOT);
}

/* ---- encendido ---------------------------------------------------------------------- */

void gc_gx_reset(gc_gx_t *gx)
{
    gc_gx_reset_video_buffer(gx);
    gx->sync_ticks = 0;
    gx->suspended = 1;
    gx->event_ticks = 0;
    gx->cycles = 0;
    gx->in_dl = 0;
    memset(gx->bp, 0, sizeof gx->bp);
    gx->bp[GX_BP_MASK] = 0xFFFFFFu;
    memset(&gx->cp, 0, sizeof gx->cp);
    memset(gx->xf, 0, sizeof gx->xf);
    gx->commands = gx->primitives = gx->vertices = gx->display_lists = 0;
    gx->unknown = gx->warnings = 0;
    memset(gx->bp_noted, 0, sizeof gx->bp_noted);
}

int gc_gx_init(gc_gx_t *gx, struct gc_system *sys)
{
    gx->sys = sys;
    gx->vbuf = malloc(GC_GX_VBUF_SIZE);
    if (!gx->vbuf)
        return -1;
    gc_gx_reset(gx);
    return 0;
}

void gc_gx_free(gc_gx_t *gx)
{
    free(gx->vbuf);
    gx->vbuf = NULL;
}
