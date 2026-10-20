/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008-2010 Dolphin Emulator Project
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
#include "hw/exi.h"
#include "hw/pi.h"

/* El chip del IPL: espacios de direcciones (EXI_DeviceIPL.h de Dolphin) */
#define IPL_ROM_SIZE   0x200000u
#define IPL_SRAM_BASE  0x800000u
#define IPL_UART_BASE  0x800400u
#define IPL_UART_SIZE  0x50u
#define IPL_EUART_BASE 0xC00000u
#define IPL_EUART_SIZE 8u

enum { DEV_NONE, DEV_IPL };

/* Qué hay en cada selección de chip: solo el IPL en el canal 0, CS 2 */
static int device(int channel, uint32_t cs)
{
    return channel == 0 && cs == 2 ? DEV_IPL : DEV_NONE;
}

/* ---- SRAM ------------------------------------------------------------------------- */

void gc_sram_fix_checksums(uint8_t sram[GC_SRAM_SIZE])
{
    uint16_t sum = 0, inv = 0;
    for (uint32_t k = 0x10; k < 0x18; k += 2) {
        const uint16_t v = (uint16_t)(sram[k] << 8 | sram[k + 1]);
        sum = (uint16_t)(sum + v);
        inv = (uint16_t)(inv + (uint16_t)~v);
    }
    sram[4] = (uint8_t)(sum >> 8);
    sram[5] = (uint8_t)sum;
    sram[6] = (uint8_t)(inv >> 8);
    sram[7] = (uint8_t)inv;
}

/* Los valores de Dolphin (Sram.cpp y el constructor de CEXIIPL) */
static void sram_defaults(uint8_t s[GC_SRAM_SIZE])
{
    memset(s, 0, GC_SRAM_SIZE);
    s[0x16] = 0;                                   /* idioma: inglés */
    s[0x17] = 0x20 | 0x08 | 0x04;                  /* configuración hecha, estéreo */
    memcpy(s + 0x18, "DOLPHINSLOTA", 12);          /* ID de flash de las tarjetas */
    memcpy(s + 0x24, "DOLPHINSLOTB", 12);
    s[0x3E] = 0x6E;
    s[0x3F] = 0x6D;
    gc_sram_fix_checksums(s);
}

uint32_t gc_exi_rtc(const gc_exi_t *exi)
{
    const gc_system_t *s = exi->sys;
    return (uint32_t)(s->rtc_boot + s->cpu.cycles / GC_CPU_CLOCK);
}

/* ---- el chip del IPL ------------------------------------------------------------- */

static void uart_byte(gc_exi_t *exi, uint8_t *data, int write)
{
    if (!write) {
        *data = 0;                                 /* nada en la cola */
        return;
    }
    if (*data == '\r') {
        gc_hle_output(exi->sys, "\n", 1);          /* la línea va al log como OSReport */
    } else if (*data != 0) {
        const char c = (char)*data;
        gc_hle_output(exi->sys, &c, 1);
    }
}

static void ipl_transfer_byte(gc_exi_t *exi, uint8_t *data)
{
    if (exi->ipl_cmd_bytes < 4) {                  /* los 4 primeros bytes son el comando */
        exi->ipl_cmd = exi->ipl_cmd << 8 | *data;
        *data = 0xFF;
        if (++exi->ipl_cmd_bytes == 4) {           /* el reloj se toma al recibir el comando */
            const uint32_t rtc = gc_exi_rtc(exi);
            exi->sram[0] = (uint8_t)(rtc >> 24);
            exi->sram[1] = (uint8_t)(rtc >> 16);
            exi->sram[2] = (uint8_t)(rtc >> 8);
            exi->sram[3] = (uint8_t)rtc;
        }
        return;
    }
    const int write = (exi->ipl_cmd >> 31) & 1;
    const uint32_t addr = (exi->ipl_cmd >> 6) & 0x1FFFFFFu;
    if (addr < IPL_ROM_SIZE) {
        if (!write) {
            exi->ipl_cursor++;
            *data = 0;                             /* sin ROM del IPL (D-005) */
        }
    } else if (addr >= IPL_SRAM_BASE && addr < IPL_SRAM_BASE + GC_SRAM_SIZE) {
        const uint32_t k = (addr - IPL_SRAM_BASE + exi->ipl_cursor++) % GC_SRAM_SIZE;
        if (write)
            exi->sram[k] = *data;
        else
            *data = exi->sram[k];
    } else if (addr >= IPL_UART_BASE && addr < IPL_UART_BASE + IPL_UART_SIZE) {
        if (addr == IPL_UART_BASE)
            uart_byte(exi, data, write);
    } else if (addr >= IPL_EUART_BASE && addr < IPL_EUART_BASE + IPL_EUART_SIZE) {
        if (addr == IPL_EUART_BASE + 4)
            uart_byte(exi, data, write);
    } else {
        GC_LOGW("exi: IPL, direccion 0x%07x desconocida", (unsigned)addr);
    }
}

static void transfer_byte(gc_exi_t *exi, int dev, uint8_t *data)
{
    if (dev == DEV_IPL)
        ipl_transfer_byte(exi, data);
}

/* ---- canales -------------------------------------------------------------------- */

static uint32_t cs_of(const gc_exi_channel_t *ch)
{
    return (ch->csr & EXI_CSR_CS_MASK) >> EXI_CSR_CS_SHIFT;
}

static void update_interrupts(gc_exi_t *exi)
{
    exi->ch[2].csr &= ~EXI_CSR_EXIINT;             /* sin dispositivo en el puerto serie 1 */
    int irq = 0;
    for (int c = 0; c < GC_EXI_CHANNELS; c++) {
        const uint32_t s = exi->ch[c].csr;
        if (((s & EXI_CSR_EXIINT) && (s & EXI_CSR_EXIINTMASK)) ||
            ((s & EXI_CSR_TCINT) && (s & EXI_CSR_TCINTMASK)) ||
            ((s & EXI_CSR_EXTINT) && (s & EXI_CSR_EXTINTMASK)))
            irq = 1;
    }
    gc_pi_set_interrupt(&exi->sys->pi, PI_INT_EXI, irq);
}

static void write_csr(gc_exi_t *exi, int c, uint32_t v)
{
    gc_exi_channel_t *ch = &exi->ch[c];
    uint32_t s = ch->csr;
    s = (s & ~(EXI_CSR_EXIINTMASK | EXI_CSR_TCINTMASK | EXI_CSR_CLK_MASK)) |
        (v & (EXI_CSR_EXIINTMASK | EXI_CSR_TCINTMASK | EXI_CSR_CLK_MASK));
    if (v & EXI_CSR_EXIINT)
        s &= ~EXI_CSR_EXIINT;
    if (v & EXI_CSR_TCINT)
        s &= ~EXI_CSR_TCINT;
    if (c < 2) {
        s = (s & ~EXI_CSR_EXTINTMASK) | (v & EXI_CSR_EXTINTMASK);
        if (v & EXI_CSR_EXTINT)
            s &= ~EXI_CSR_EXTINT;
    }
    if (c == 0)
        s = (s & ~EXI_CSR_ROMDIS) | (v & EXI_CSR_ROMDIS);
    /* El dispositivo cuya selección cambió se entera (el IPL vuelve a esperar comando) */
    const uint32_t old_cs = cs_of(ch), new_cs = (v & EXI_CSR_CS_MASK) >> EXI_CSR_CS_SHIFT;
    s = (s & ~EXI_CSR_CS_MASK) | (v & EXI_CSR_CS_MASK);
    ch->csr = s;
    if (device(c, old_cs ^ new_cs) == DEV_IPL && new_cs) {
        exi->ipl_cmd_bytes = 0;
        exi->ipl_cursor = 0;
    }
    update_interrupts(exi);
}

static void start(gc_exi_t *exi, int c)
{
    gc_exi_channel_t *ch = &exi->ch[c];
    const uint32_t cs = cs_of(ch);
    if (cs != 1 && cs != 2 && cs != 4)
        return;                                    /* sin chip elegido: TSTART queda */
    const int dev = device(c, cs);
    const uint32_t rw = (ch->cr >> 2) & 3;
    gc_mem_t *m = &exi->sys->mem;
    if (!(ch->cr & EXI_CR_DMA)) {
        const uint32_t len = ((ch->cr >> 4) & 3) + 1;
        if (rw == 0) {                             /* lectura: los bytes van de arriba abajo */
            uint32_t r = 0;
            for (uint32_t k = 0; k < len; k++) {
                uint8_t b = 0;
                transfer_byte(exi, dev, &b);
                r |= (uint32_t)b << (24 - 8 * k);
            }
            ch->data = r;
        } else if (rw == 1) {
            uint32_t d = ch->data;
            for (uint32_t k = 0; k < len; k++, d <<= 8) {
                uint8_t b = (uint8_t)(d >> 24);
                transfer_byte(exi, dev, &b);
            }
        }                                          /* lectura y escritura: no hace nada */
    } else {
        if (rw == 0) {
            for (uint32_t k = 0; k < ch->length; k++) {
                uint8_t b = 0;
                transfer_byte(exi, dev, &b);
                gc_write8(m, ch->mar + k, b);
            }
        } else if (rw == 1) {
            for (uint32_t k = 0; k < ch->length; k++) {
                uint8_t b = gc_read8(m, ch->mar + k);
                transfer_byte(exi, dev, &b);
            }
        } else {
            GC_LOGW("exi: DMA de tipo %u en el canal %d", (unsigned)rw, c);
        }
    }
    ch->cr &= ~EXI_CR_TSTART;
    ch->csr |= EXI_CSR_TCINT;                      /* termina en el momento */
    update_interrupts(exi);
}

static uint32_t exi_read(void *ctx, uint32_t off, int size)
{
    gc_exi_t *exi = ctx;
    if (size != 4 || off >= 0x14 * GC_EXI_CHANNELS) {
        GC_LOGW("exi: lectura de %d bits en 0x%02x", size * 8, (unsigned)off);
        return 0;
    }
    gc_exi_channel_t *ch = &exi->ch[off / 0x14];
    switch (off % 0x14) {
    case EXI_CSR:
        ch->csr &= ~EXI_CSR_EXT;                   /* ranuras de tarjeta vacías */
        return ch->csr;
    case EXI_MAR:    return ch->mar;
    case EXI_LENGTH: return ch->length;
    case EXI_CR:     return ch->cr;
    default:         return ch->data;
    }
}

static void exi_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_exi_t *exi = ctx;
    if (size != 4 || off >= 0x14 * GC_EXI_CHANNELS) {
        GC_LOGW("exi: escritura de %d bits en 0x%02x ignorada", size * 8, (unsigned)off);
        return;
    }
    const int c = (int)(off / 0x14);
    gc_exi_channel_t *ch = &exi->ch[c];
    switch (off % 0x14) {
    case EXI_CSR:    write_csr(exi, c, v); break;
    case EXI_MAR:    ch->mar = v; break;
    case EXI_LENGTH: ch->length = v; break;
    case EXI_CR:
        ch->cr = v;
        if (v & EXI_CR_TSTART)
            start(exi, c);
        break;
    default:         ch->data = v; break;
    }
}

void gc_exi_reset(gc_exi_t *exi)
{
    struct gc_system *sys = exi->sys;
    memset(exi, 0, sizeof *exi);
    exi->sys = sys;
    exi->ch[0].csr = EXI_CSR_EXTINT;               /* como Dolphin al armar los canales */
    exi->ch[1].csr = EXI_CSR_EXTINT | 1u << EXI_CSR_CS_SHIFT;
    sram_defaults(exi->sram);
    update_interrupts(exi);
}

void gc_exi_init(gc_exi_t *exi, struct gc_system *sys)
{
    exi->sys = sys;
    gc_exi_reset(exi);
    gc_mmio_register(&sys->mem.io, "EXI", GC_MMIO_EXI, 0x100, exi_read, exi_write, exi);
}
