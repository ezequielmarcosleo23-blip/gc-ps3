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
#include "hw/di.h"
#include "hw/pi.h"

#define COMMAND_CYCLES (300u * (GC_CPU_CLOCK / 1000000u))   /* 300 µs */

enum { STATE_READY = 0, STATE_COVER_OPEN = 2 };

/* Comandos (DICommand de Dolphin) */
enum {
    CMD_INQUIRY = 0x12, CMD_UNKNOWN55 = 0x55, CMD_REPORT_KEY = 0xA4, CMD_READ = 0xA8, CMD_SEEK = 0xAB,
    CMD_READ_BCA = 0xDA, CMD_REQUEST_ERROR = 0xE0, CMD_AUDIO_STREAM = 0xE1,
    CMD_AUDIO_STATUS = 0xE2, CMD_STOP_MOTOR = 0xE3, CMD_AUDIO_CONFIG = 0xE4, CMD_DEBUG_UNLOCK = 0xFF,
};

static void update_interrupts(gc_di_t *di)
{
    const uint32_t s = di->sr, c = di->cvr;
    const int irq = ((s & DI_SR_DEINT) && (s & DI_SR_DEINTMASK)) ||
                    ((s & DI_SR_TCINT) && (s & DI_SR_TCINTMASK)) ||
                    ((s & DI_SR_BRKINT) && (s & DI_SR_BRKINTMASK)) ||
                    ((c & DI_CVR_INT) && (c & DI_CVR_INTMASK));
    gc_pi_set_interrupt(&di->sys->pi, PI_INT_DI, irq);
}

static void finish(void *ctx, uint64_t when, uint64_t late)
{
    gc_di_t *di = ctx;
    (void)when;
    (void)late;
    if (!di->pending_deint) {                  /* lo transferido avanza la dirección */
        di->mar += di->length;
        di->length = 0;
    }
    if (di->cr & DI_CR_TSTART) {
        di->cr &= ~DI_CR_TSTART;
        di->sr |= di->pending_deint ? DI_SR_DEINT : DI_SR_TCINT;
        update_interrupts(di);
    }
}

static void execute(gc_di_t *di)
{
    gc_mem_t *m = &di->sys->mem;
    const uint32_t op = di->cmd[0] >> 24;
    int deint = 0;
    if (op != CMD_REQUEST_ERROR)
        di->error = 0;

    switch (op) {
    case CMD_INQUIRY:                          /* revisión, fecha y versión de la unidad */
        gc_write32(m, di->mar, 0x00000002u);
        gc_write32(m, di->mar + 4, 0x20060526u);
        gc_write32(m, di->mar + 8, 0x41000000u);
        break;
    case CMD_READ:
        if ((di->cmd[0] & 0xFFu) == 0x00 || (di->cmd[0] & 0xFFu) == 0x40) {
            di->error = DI_ERROR_NO_MEDIUM;    /* sin disco no hay qué leer */
            deint = 1;
        }
        break;
    case CMD_SEEK:
    case CMD_STOP_MOTOR:
    case CMD_DEBUG_UNLOCK:
        break;
    case CMD_READ_BCA:
        for (uint32_t k = 0; k < 0x40; k++)
            gc_write8(m, di->mar + k, k == 0x33 ? 1 : 0);
        break;
    case CMD_REQUEST_ERROR: {
        const uint32_t state = di->state == STATE_READY ? 0 : di->state - 1;
        di->imm = state << 24 | di->error;
        di->error = 0;
        break;
    }
    case CMD_AUDIO_STREAM:
    case CMD_AUDIO_STATUS:
    case CMD_AUDIO_CONFIG:
        di->error = DI_ERROR_NO_MEDIUM;
        deint = 1;
        break;
    default:                                   /* lo demás no existe en una GameCube */
        if (op != CMD_UNKNOWN55 && op != CMD_REPORT_KEY)
            GC_LOGW("di: comando 0x%08x desconocido", (unsigned)di->cmd[0]);
        di->error = DI_ERROR_INVALID_COMMAND;
        deint = 1;
        break;
    }
    di->pending_deint = deint;
    gc_sched_cancel(&di->sys->sched, finish, di);
    gc_sched_add(&di->sys->sched, di->sys->cpu.cycles + COMMAND_CYCLES, "di", finish, di);
}

static uint32_t di_read(void *ctx, uint32_t off, int size)
{
    gc_di_t *di = ctx;
    if (size != 4) {
        GC_LOGW("di: lectura de %d bits en 0x%02x", size * 8, (unsigned)off);
        return 0;
    }
    switch (off) {
    case DI_SR:      return di->sr;
    case DI_CVR:     return di->cvr;
    case DI_CMDBUF0: return di->cmd[0];
    case DI_CMDBUF1: return di->cmd[1];
    case DI_CMDBUF2: return di->cmd[2];
    case DI_MAR:     return di->mar;
    case DI_LENGTH:  return di->length;
    case DI_CR:      return di->cr;
    case DI_IMMBUF:  return di->imm;
    case DI_CFG:     return di->cfg;
    default:         return 0;
    }
}

static void di_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_di_t *di = ctx;
    if (size != 4) {
        GC_LOGW("di: escritura de %d bits en 0x%02x ignorada", size * 8, (unsigned)off);
        return;
    }
    switch (off) {
    case DI_SR: {
        const uint32_t keep = DI_SR_BRK | DI_SR_DEINTMASK | DI_SR_TCINTMASK | DI_SR_BRKINTMASK;
        di->sr = (di->sr & ~keep) | (v & keep);
        di->sr &= ~(v & (DI_SR_DEINT | DI_SR_TCINT | DI_SR_BRKINT));
        update_interrupts(di);
        break;
    }
    case DI_CVR:
        di->cvr = (di->cvr & ~DI_CVR_INTMASK) | (v & DI_CVR_INTMASK);
        if (v & DI_CVR_INT)
            di->cvr &= ~DI_CVR_INT;
        update_interrupts(di);
        break;
    case DI_CMDBUF0: di->cmd[0] = v; break;
    case DI_CMDBUF1: di->cmd[1] = v; break;
    case DI_CMDBUF2: di->cmd[2] = v; break;
    case DI_MAR:     di->mar = v & 0x03FFFFE0u; break;
    case DI_LENGTH:  di->length = v & ~0x1Fu; break;
    case DI_CR:
        di->cr = v & 7u;
        if (di->cr & DI_CR_TSTART)
            execute(di);
        break;
    case DI_IMMBUF:  di->imm = v; break;
    default:
        GC_LOGW("di: escritura a 0x%02x ignorada (0x%08x)", (unsigned)off, (unsigned)v);
        break;
    }
}

void gc_di_reset(gc_di_t *di)
{
    struct gc_system *sys = di->sys;
    memset(di, 0, sizeof *di);
    di->sys = sys;
    di->cvr = DI_CVR_OPEN;                     /* sin disco: la tapa abierta */
    di->cfg = 1;                               /* sin el descifrado de la ROM de arranque */
    di->state = STATE_COVER_OPEN;
    gc_sched_cancel(&sys->sched, finish, di);
    update_interrupts(di);
}

void gc_di_init(gc_di_t *di, struct gc_system *sys)
{
    di->sys = sys;
    gc_mmio_register(&sys->mem.io, "DI", GC_MMIO_DI, 0x100, di_read, di_write, di);
}
