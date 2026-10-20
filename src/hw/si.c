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
#include "hw/pi.h"
#include "hw/si.h"

/* Comandos del búfer y directos (SI_Device.h de Dolphin) */
enum {
    CMD_STATUS = 0x00, CMD_SET_GAME_ID = 0x1D, CMD_DIRECT = 0x40, CMD_ORIGIN = 0x41,
    CMD_RECALIBRATE = 0x42, CMD_RESET = 0xFF,
};
#define DIRECT_WRITE 0x40u                 /* comando por SIOUT: modo y vibración */

#define IN_HI_ERRSTAT  0x80000000u
#define IN_HI_ERRLATCH 0x40000000u

static void update_interrupts(gc_si_t *si)
{
    int rdst = 0;
    for (int c = 0; c < GC_SI_PORTS; c++)
        if (si->sr & SI_SR_RDST(c))
            rdst = 1;
    if (rdst)
        si->comcsr |= SI_COMCSR_RDSTINT;
    else
        si->comcsr &= ~SI_COMCSR_RDSTINT;
    const int irq = ((si->comcsr & SI_COMCSR_RDSTINT) && (si->comcsr & SI_COMCSR_RDSTINTMSK)) ||
                    ((si->comcsr & SI_COMCSR_TCINT) && (si->comcsr & SI_COMCSR_TCINTMSK));
    gc_pi_set_interrupt(&si->sys->pi, PI_INT_SI, irq);
}

/* ---- el mando ------------------------------------------------------------------- */

/* Palabras del sondeo según el modo (GetData de Dolphin) */
static void pad_data(const gc_si_t *si, int c, uint32_t *hi, uint32_t *lo)
{
    const gc_pad_t *p = &si->pad[c];
    *hi = (uint32_t)p->stick_y | (uint32_t)p->stick_x << 8 |
          (uint32_t)(p->buttons | GC_PAD_USE_ORIGIN) << 16;
    switch (si->mode[c]) {
    case 1:
        *lo = (uint32_t)(p->analog_b >> 4) | (uint32_t)(p->analog_a >> 4) << 4 |
              (uint32_t)p->trig_r << 8 | (uint32_t)p->trig_l << 16 |
              (uint32_t)(p->sub_y >> 4) << 24 | (uint32_t)(p->sub_x >> 4) << 28;
        break;
    case 2:
        *lo = (uint32_t)p->analog_b | (uint32_t)p->analog_a << 8 |
              (uint32_t)(p->trig_r >> 4) << 16 | (uint32_t)(p->trig_l >> 4) << 20 |
              (uint32_t)(p->sub_y >> 4) << 24 | (uint32_t)(p->sub_x >> 4) << 28;
        break;
    case 3:
        *lo = (uint32_t)p->trig_r | (uint32_t)p->trig_l << 8 | (uint32_t)p->sub_y << 16 |
              (uint32_t)p->sub_x << 24;
        break;
    case 4:
        *lo = (uint32_t)p->analog_b | (uint32_t)p->analog_a << 8 | (uint32_t)p->sub_y << 16 |
              (uint32_t)p->sub_x << 24;
        break;
    default:                               /* 0, 5, 6 y 7 */
        *lo = (uint32_t)(p->analog_b >> 4) | (uint32_t)(p->analog_a >> 4) << 4 |
              (uint32_t)(p->trig_r >> 4) << 8 | (uint32_t)(p->trig_l >> 4) << 12 |
              (uint32_t)p->sub_y << 16 | (uint32_t)p->sub_x << 24;
        break;
    }
}

/* Un comando por el búfer: largo de la respuesta, -1 sin respuesta, 0 si no
 * termina (Dolphin la reintenta para siempre; acá queda en curso). */
static int run_buffer(gc_si_t *si, int c)
{
    if (!si->pad[c].connected)
        return -1;
    uint8_t *b = si->buf;
    switch (b[0]) {
    case CMD_STATUS:
    case CMD_RESET:
        b[0] = (uint8_t)(SI_ID_GC_CONTROLLER >> 24);
        b[1] = (uint8_t)(SI_ID_GC_CONTROLLER >> 16);
        b[2] = (uint8_t)(SI_ID_GC_CONTROLLER >> 8);
        return 3;
    case CMD_DIRECT: {
        uint32_t hi, lo;
        pad_data(si, c, &hi, &lo);
        for (int i = 0; i < 4; i++) {
            b[i] = (uint8_t)(hi >> (24 - 8 * i));
            b[i + 4] = (uint8_t)(lo >> (24 - 8 * i));
        }
        return 8;
    }
    case CMD_ORIGIN:
    case CMD_RECALIBRATE:
        memcpy(b, si->origin[c], sizeof si->origin[c]);
        return (int)sizeof si->origin[c];
    case CMD_SET_GAME_ID:
        return 0;
    default:
        GC_LOGW("si: comando 0x%02x desconocido en el puerto %d", (unsigned)b[0], c + 1);
        return 0;
    }
}

static void transfer(gc_si_t *si)
{
    const int c = (int)(si->comcsr >> 1) & 3;
    const int n = run_buffer(si, c);
    if (n == 0)
        return;                            /* sigue en curso */
    si->comcsr &= ~(SI_COMCSR_TSTART | SI_COMCSR_COMERR);
    if (n < 0) {
        si->comcsr |= SI_COMCSR_COMERR;
        si->sr |= SI_SR_NOREP(c);
    }
    si->comcsr |= SI_COMCSR_TCINT;
    update_interrupts(si);
}

/* Lo que se manda por SIOUT al escribir SISR[WR] (SendCommand de Dolphin) */
static void send_command(gc_si_t *si, int c, int poll_enabled)
{
    if (!si->pad[c].connected)
        return;
    const uint32_t cmd = (si->out[c] >> 16) & 0xFFu;
    if (cmd == DIRECT_WRITE) {
        if (!poll_enabled)
            si->mode[c] = (uint8_t)(si->out[c] >> 8);
    } else if (cmd != 0) {
        GC_LOGW("si: comando directo 0x%08x desconocido", (unsigned)si->out[c]);
    }
}

void gc_si_poll(gc_si_t *si)
{
    for (int c = 0; c < GC_SI_PORTS; c++) {
        const uint32_t errlatch = si->in_hi[c] & IN_HI_ERRLATCH;
        if (si->pad[c].connected) {
            pad_data(si, c, &si->in_hi[c], &si->in_lo[c]);
            si->in_hi[c] = (si->in_hi[c] & ~IN_HI_ERRLATCH) | errlatch;
            si->sr |= SI_SR_RDST(c);
        } else {
            si->sr |= SI_SR_NOREP(c);
            si->in_hi[c] |= IN_HI_ERRSTAT | IN_HI_ERRLATCH;
        }
    }
    update_interrupts(si);
}

uint32_t gc_si_poll_lines(const gc_si_t *si)
{
    return (si->poll >> 16) & 0x3FFu;
}

/* ---- registros -------------------------------------------------------------------- */

static uint32_t read32(gc_si_t *si, uint32_t off)
{
    if (off >= SI_BUF && off < SI_BUF + GC_SI_BUF_SIZE) {
        const uint8_t *b = si->buf + (off - SI_BUF);
        return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    }
    if (off < SI_POLL) {
        const int c = (int)(off / 0x0C);
        switch (off % 0x0C) {
        case SI_OUT0:
            return si->out[c];
        case SI_IN0_HI:
            si->sr &= ~SI_SR_RDST(c);            /* leer borra RDST */
            update_interrupts(si);
            return si->in_hi[c];
        default:
            si->sr &= ~SI_SR_RDST(c);
            update_interrupts(si);
            return si->in_lo[c];
        }
    }
    switch (off) {
    case SI_POLL:   return si->poll;
    case SI_COMCSR: return si->comcsr;
    case SI_SR:     return si->sr;
    case SI_EXILK:  return si->exilk;
    default:        return 0;
    }
}

static void write32(gc_si_t *si, uint32_t off, uint32_t v)
{
    if (off >= SI_BUF && off < SI_BUF + GC_SI_BUF_SIZE) {
        uint8_t *b = si->buf + (off - SI_BUF);
        b[0] = (uint8_t)(v >> 24);
        b[1] = (uint8_t)(v >> 16);
        b[2] = (uint8_t)(v >> 8);
        b[3] = (uint8_t)v;
        return;
    }
    if (off < SI_POLL) {
        const int c = (int)(off / 0x0C);
        switch (off % 0x0C) {
        case SI_OUT0:   si->out[c] = v; break;
        case SI_IN0_HI: si->in_hi[c] = v; break;
        default:        si->in_lo[c] = v; break;
        }
        return;
    }
    switch (off) {
    case SI_POLL:
        si->poll = v;
        break;
    case SI_COMCSR: {
        /* Se guardan canal, largos y máscaras; INT se borra con 1; TSTART arranca */
        const uint32_t keep = 0x00000006u | 0x00007F00u | 0x007F0000u | SI_COMCSR_RDSTINTMSK |
                              SI_COMCSR_TCINTMSK;
        si->comcsr = (si->comcsr & ~keep) | (v & keep);
        if (v & SI_COMCSR_RDSTINT)
            si->comcsr &= ~SI_COMCSR_RDSTINT;
        if (v & SI_COMCSR_TCINT)
            si->comcsr &= ~SI_COMCSR_TCINT;
        if (v & SI_COMCSR_TSTART) {
            si->comcsr |= SI_COMCSR_TSTART;
            transfer(si);
        }
        if (!(si->comcsr & SI_COMCSR_TSTART))
            update_interrupts(si);
        break;
    }
    case SI_SR:
        /* NOREP, COLL, OVRUN y UNRUN de cada canal se borran con 1 */
        si->sr &= ~(v & 0x0F0F0F0Fu);
        if (v & SI_SR_WR) {
            for (int c = 0; c < GC_SI_PORTS; c++)
                send_command(si, c, (int)(si->poll >> (7 - c)) & 1);
            si->sr &= ~(SI_SR_WR | 0x10101010u);      /* WR y WRST */
        }
        break;
    case SI_EXILK:
        si->exilk = v;
        break;
    default:
        GC_LOGW("si: escritura a 0x%02x sin implementar (0x%08x)", (unsigned)off, (unsigned)v);
        break;
    }
}

/* Registros de 32 bits; el búfer también de a 16. */
static uint32_t si_read(void *ctx, uint32_t off, int size)
{
    gc_si_t *si = ctx;
    if (size == 4)
        return read32(si, off & ~3u);
    if (size == 2 && off >= SI_BUF && off < SI_BUF + GC_SI_BUF_SIZE)
        return (uint32_t)si->buf[off - SI_BUF] << 8 | si->buf[off - SI_BUF + 1];
    GC_LOGW("si: lectura de %d bits en 0x%02x", size * 8, (unsigned)off);
    return 0;
}

static void si_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_si_t *si = ctx;
    if (size == 4) {
        write32(si, off & ~3u, v);
    } else if (size == 2 && off >= SI_BUF && off < SI_BUF + GC_SI_BUF_SIZE) {
        si->buf[off - SI_BUF] = (uint8_t)(v >> 8);
        si->buf[off - SI_BUF + 1] = (uint8_t)v;
    } else {
        GC_LOGW("si: escritura de %d bits en 0x%02x ignorada", size * 8, (unsigned)off);
    }
}

void gc_si_reset(gc_si_t *si)
{
    struct gc_system *sys = si->sys;
    memset(si, 0, sizeof *si);
    si->sys = sys;
    for (int c = 0; c < GC_SI_PORTS; c++) {
        gc_pad_t *p = &si->pad[c];
        p->connected = c == 0;             /* mando en el puerto 1, como Dolphin */
        p->stick_x = p->stick_y = p->sub_x = p->sub_y = 0x80;
        si->mode[c] = 3;
        uint8_t *o = si->origin[c];        /* botones (2 bytes), palancas, gatillos, 2 más */
        o[2] = o[3] = o[4] = o[5] = 0x80;
    }
    si->poll = 492u << 16;                 /* X = 492, como Dolphin al encender */
}

void gc_si_init(gc_si_t *si, struct gc_system *sys)
{
    si->sys = sys;
    gc_si_reset(si);
    gc_mmio_register(&sys->mem.io, "SI", GC_MMIO_SI, 0x100, si_read, si_write, si);
}
