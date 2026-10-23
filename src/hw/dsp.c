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
#include "hw/dsp.h"
#include "hw/pi.h"

#define HLE_BITS     0x0C07u              /* los bits de DSPCSR que maneja el DSP */
#define INT_BITS     (DSP_CSR_AIDINT | DSP_CSR_ARINT | DSP_CSR_DSPINT)
#define CYCLES_PER_AR_BLOCK 246u          /* por cada 32 bytes del DMA de la ARAM */
#define AID_START_DELAY     200u          /* primer aviso del DMA de audio */
#define INITCODE_TB         130u          /* DSPInitCode dura 130 ticks de timebase */

enum { UCODE_ROM, UCODE_INIT, UCODE_NONE };

static void update_interrupts(gc_dsp_t *dsp)
{
    const int irq = ((dsp->csr >> 1) & dsp->csr & INT_BITS) != 0;
    gc_pi_set_interrupt(&dsp->sys->pi, PI_INT_DSP, irq);
}

static void raise(gc_dsp_t *dsp, uint16_t bit)
{
    dsp->csr |= bit;
    update_interrupts(dsp);
}

/* ---- correos ----------------------------------------------------------------------- */

static void push_mail(gc_dsp_t *dsp, uint32_t mail)
{
    if (dsp->mail_count < DSP_MAIL_QUEUE)
        dsp->mail[dsp->mail_count++] = mail;
    else
        GC_LOGW("dsp: cola de correos llena, se pierde 0x%08x", (unsigned)mail);
}

static void set_ucode(gc_dsp_t *dsp, int which)
{
    dsp->mail_count = 0;
    dsp->rom_param = 0;
    dsp->ucode = which;
    if (which == UCODE_ROM)
        push_mail(dsp, DSP_MAIL_ROM);
    else if (which == UCODE_INIT)
        push_mail(dsp, DSP_MAIL_INIT);
}

/* El microcódigo ROM (ROM.cpp de Dolphin): contesta 0xFEEExxxx a cada correo
 * salvo los 0x80F3xxxx, que piden cargar un microcódigo. */
static void mail_to_dsp(gc_dsp_t *dsp, uint32_t mail)
{
    if (dsp->ucode != UCODE_ROM)
        return;
    if (dsp->rom_param == 0) {
        if ((mail & 0xFFFF0000u) != 0x80F30000u)
            push_mail(dsp, 0xFEEE0000u | (mail & 0xFFFFu));
        else
            dsp->rom_param = mail;
        return;
    }
    if (dsp->rom_param == 0x80F3D001u) {
        GC_LOGW("dsp: carga de microcodigo sin soporte (fase de audio)");
        set_ucode(dsp, UCODE_NONE);
        return;
    }
    dsp->rom_param = 0;
}

static int halted(const gc_dsp_t *dsp)
{
    return (dsp->csr & DSP_CSR_HALT) != 0;
}

static uint16_t read_mail_hi(gc_dsp_t *dsp)
{
    if (!halted(dsp) && dsp->mail_count)
        dsp->last_mail = dsp->mail[0];
    return (uint16_t)(dsp->last_mail >> 16);
}

static uint16_t read_mail_lo(gc_dsp_t *dsp)
{
    if (!halted(dsp) && dsp->mail_count) {
        dsp->last_mail = dsp->mail[0];
        dsp->mail_count--;
        memmove(dsp->mail, dsp->mail + 1, dsp->mail_count * sizeof dsp->mail[0]);
    }
    dsp->last_mail &= ~0x80000000u;
    return (uint16_t)dsp->last_mail;
}

/* ---- DSPCSR ---------------------------------------------------------------------- */

static uint16_t read_csr(gc_dsp_t *dsp)
{
    if ((dsp->csr & DSP_CSR_INITCODE) && gekko_tb(&dsp->sys->cpu) >= dsp->initcode_clear_tb)
        dsp->csr &= ~DSP_CSR_INITCODE;
    return dsp->csr;
}

static void write_csr(gc_dsp_t *dsp, uint16_t v)
{
    /* Lo que hace el DSP por HLE */
    uint16_t hle = v;
    if (hle & DSP_CSR_RESET) {
        set_ucode(dsp, UCODE_ROM);
        hle &= ~DSP_CSR_RESET;                     /* el reset termina en el acto */
    }
    if ((dsp->csr & DSP_CSR_INIT) && !(hle & DSP_CSR_INIT)) {
        set_ucode(dsp, UCODE_INIT);
        hle |= DSP_CSR_INITCODE;
        dsp->initcode_clear_tb = gekko_tb(&dsp->sys->cpu) + INITCODE_TB;
    }
    if (v & DSP_CSR_RESET)
        dsp->audio_control = 0;
    /* Las interrupciones se borran con 1; las máscaras y el resto se guardan */
    uint16_t csr = (uint16_t)((dsp->csr & (DSP_CSR_DMA | INT_BITS)) | (hle & HLE_BITS) |
                              (v & (DSP_CSR_AIDMASK | DSP_CSR_ARMASK | DSP_CSR_DSPMASK | 0xF000u)));
    csr &= (uint16_t)~(v & INT_BITS);
    dsp->csr = csr;
    if (v & 0xF000u)
        GC_LOGW("dsp: DSPCSR con bits de relleno (0x%04x)", (unsigned)v);
    update_interrupts(dsp);
}

/* ---- DMA de la ARAM ----------------------------------------------------------------- */

static void aram_done(void *ctx, uint64_t when, uint64_t late)
{
    gc_dsp_t *dsp = ctx;
    (void)when;
    (void)late;
    dsp->csr &= ~DSP_CSR_DMA;
    raise(dsp, DSP_CSR_ARINT);
}

static void aram_dma(gc_dsp_t *dsp)
{
    gc_system_t *s = dsp->sys;
    gc_mem_t *m = &s->mem;
    const int to_ram = (dsp->ar_cnt >> 31) & 1;
    uint32_t count = dsp->ar_cnt & 0x7FFFFFFFu;

    dsp->csr |= DSP_CSR_DMA;
    gc_sched_add(&s->sched, s->cpu.cycles + (uint64_t)(count / 32) * CYCLES_PER_AR_BLOCK, "aram",
                 aram_done, dsp);

    dsp->ar_ar &= 0x3FFFFFFu;
    dsp->ar_mm &= 0x3FFFFFFu;
    if (dsp->ar_ar >= GC_ARAM_SIZE) {
        GC_LOGW("dsp: DMA de la ARAM fuera de los 16 MB (0x%08x)", (unsigned)dsp->ar_ar);
        dsp->ar_cnt &= 0x80000000u;
        return;
    }
    const uint32_t mask = GC_ARAM_SIZE - 1;
    const int mirror = (dsp->ar_info & 0xF) == 4;  /* modo 4: lo bajo se copia también +4 MB */
    while (count) {
        for (uint32_t k = 0; k < 8; k++) {
            if (to_ram) {
                gc_write8(m, dsp->ar_mm + k, m->aram[(dsp->ar_ar + k) & mask]);
            } else {
                const uint8_t b = gc_read8(m, dsp->ar_mm + k);
                if (mirror && dsp->ar_ar < 0x400000u)
                    m->aram[(dsp->ar_ar + 0x400000u + k) & mask] = b;
                m->aram[(dsp->ar_ar + k) & mask] = b;
            }
        }
        dsp->ar_mm += 8;
        dsp->ar_ar += 8;
        count = count > 8 ? count - 8 : 0;
    }
    dsp->ar_cnt &= 0x80000000u;                    /* el contador queda en 0 */
}

/* ---- DMA de audio ------------------------------------------------------------------ */

static void aid_event(void *ctx, uint64_t when, uint64_t late)
{
    (void)when;
    (void)late;
    raise(ctx, DSP_CSR_AIDINT);
}

/* Ciclos por bloque de 32 bytes según la frecuencia de AID (AICR[AIDFR]) */
static uint64_t audio_period(const gc_dsp_t *dsp)
{
    const int k32 = (dsp->sys->ai.control >> 6) & 1;
    return k32 ? 121392u : 80928u;
}

static void audio_event(void *ctx, uint64_t when, uint64_t late)
{
    gc_dsp_t *dsp = ctx;
    (void)late;
    if (dsp->audio_control & 0x8000u) {
        dsp->audio_dma_blocks++;
        if (dsp->audio_left) {
            dsp->audio_left--;
            dsp->audio_current += 32;
        }
        if (dsp->audio_left == 0) {
            dsp->audio_current = dsp->audio_start;
            dsp->audio_left = dsp->audio_control & 0x7FFFu;
            raise(dsp, DSP_CSR_AIDINT);
        }
    }
    gc_sched_add(&dsp->sys->sched, when + audio_period(dsp), "audio", audio_event, dsp);
}

/* ---- registros ---------------------------------------------------------------------- */

static uint16_t read_reg(gc_dsp_t *dsp, uint32_t off)
{
    switch (off) {
    case DSP_MAIL_TO_HI:     return (uint16_t)(dsp->cpu_mail >> 16);
    case DSP_MAIL_TO_LO:     return (uint16_t)dsp->cpu_mail;
    case DSP_MAIL_FROM_HI:   return read_mail_hi(dsp);
    case DSP_MAIL_FROM_LO:   return read_mail_lo(dsp);
    case DSP_CSR:            return read_csr(dsp);
    case AR_INFO:            return dsp->ar_info;
    case AR_MODE:            return dsp->ar_mode;
    case AR_REFRESH:         return dsp->ar_refresh;
    case AR_MMADDR_HI:       return (uint16_t)(dsp->ar_mm >> 16);
    case AR_MMADDR_LO:       return (uint16_t)dsp->ar_mm;
    case AR_ARADDR_HI:       return (uint16_t)(dsp->ar_ar >> 16);
    case AR_ARADDR_LO:       return (uint16_t)dsp->ar_ar;
    case AR_CNT_HI:          return (uint16_t)(dsp->ar_cnt >> 16);
    case AR_CNT_LO:          return (uint16_t)dsp->ar_cnt;
    case AUDIO_DMA_START_HI: return (uint16_t)(dsp->audio_start >> 16);
    case AUDIO_DMA_START_LO: return (uint16_t)dsp->audio_start;
    case AUDIO_DMA_CONTROL:  return dsp->audio_control;
    case AUDIO_DMA_LEFT:     return dsp->audio_left ? (uint16_t)(dsp->audio_left - 1) : 0;
    default:
        GC_LOGW("dsp: lectura de 0x%02x sin implementar", (unsigned)off);
        return 0;
    }
}

static void set_hi(uint32_t *r, uint16_t v, uint16_t mask)
{
    *r = (*r & 0xFFFFu) | (uint32_t)(v & mask) << 16;
}

static void set_lo(uint32_t *r, uint16_t v, uint16_t mask)
{
    *r = (*r & 0xFFFF0000u) | (v & mask);
}

static void write_reg(gc_dsp_t *dsp, uint32_t off, uint16_t v)
{
    switch (off) {
    case DSP_MAIL_TO_HI:
        set_hi(&dsp->cpu_mail, v, 0xFFFF);
        break;
    case DSP_MAIL_TO_LO:
        set_lo(&dsp->cpu_mail, v, 0xFFFF);
        mail_to_dsp(dsp, dsp->cpu_mail);
        dsp->cpu_mail &= 0x7FFFFFFFu;              /* el DSP ya lo leyó */
        break;
    case DSP_CSR:            write_csr(dsp, v); break;
    case AR_INFO:            dsp->ar_info = v & 0x7F; break;
    case AR_REFRESH:         dsp->ar_refresh = v & 0x7FF; break;
    case AR_MMADDR_HI:       set_hi(&dsp->ar_mm, v, 0x03FF); break;
    case AR_MMADDR_LO:       set_lo(&dsp->ar_mm, v, 0xFFE0); break;
    case AR_ARADDR_HI:       set_hi(&dsp->ar_ar, v, 0x03FF); break;
    case AR_ARADDR_LO:       set_lo(&dsp->ar_ar, v, 0xFFE0); break;
    case AR_CNT_HI:          set_hi(&dsp->ar_cnt, v, 0x83FF); break;
    case AR_CNT_LO:
        set_lo(&dsp->ar_cnt, v, 0xFFE0);
        aram_dma(dsp);
        break;
    case AUDIO_DMA_START_HI: set_hi(&dsp->audio_start, v, 0x03FF); break;
    case AUDIO_DMA_START_LO: set_lo(&dsp->audio_start, v, 0xFFE0); break;
    case AUDIO_DMA_CONTROL: {
        const int was_on = (dsp->audio_control & 0x8000u) != 0;
        dsp->audio_control = v;
        if (!was_on && (v & 0x8000u)) {
            dsp->audio_current = dsp->audio_start;
            dsp->audio_left = v & 0x7FFFu;
            gc_sched_add(&dsp->sys->sched, dsp->sys->cpu.cycles + AID_START_DELAY, "aid",
                         aid_event, dsp);
        }
        break;
    }
    default:
        GC_LOGW("dsp: escritura a 0x%02x ignorada (0x%04x)", (unsigned)off, (unsigned)v);
        break;
    }
}

/* Registros de 16 bits; los accesos de 32 son dos, la mitad alta primero. */
static uint32_t dsp_read(void *ctx, uint32_t off, int size)
{
    gc_dsp_t *dsp = ctx;
    if (size == 4)
        return (uint32_t)read_reg(dsp, off & ~3u) << 16 | read_reg(dsp, (off & ~3u) + 2);
    if (size == 2)
        return read_reg(dsp, off & ~1u);
    GC_LOGW("dsp: lectura de 8 bits en 0x%02x", (unsigned)off);
    return 0;
}

static void dsp_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_dsp_t *dsp = ctx;
    if (size == 4) {
        write_reg(dsp, off & ~3u, (uint16_t)(v >> 16));
        write_reg(dsp, (off & ~3u) + 2, (uint16_t)v);
    } else if (size == 2) {
        write_reg(dsp, off & ~1u, (uint16_t)v);
    } else {
        GC_LOGW("dsp: escritura de 8 bits en 0x%02x ignorada", (unsigned)off);
    }
}

void gc_dsp_reset(gc_dsp_t *dsp)
{
    struct gc_system *sys = dsp->sys;
    memset(dsp, 0, sizeof *dsp);
    dsp->sys = sys;
    dsp->csr = DSP_CSR_HALT | DSP_CSR_INIT;        /* detenido, sin inicializar */
    dsp->ar_mode = 1;                              /* controlador de la ARAM listo */
    dsp->ar_refresh = 156;
    set_ucode(dsp, UCODE_ROM);                     /* 0x8071FEED espera a que arranque */
    update_interrupts(dsp);
    gc_sched_cancel(&sys->sched, aram_done, dsp);
    gc_sched_cancel(&sys->sched, aid_event, dsp);
    gc_sched_cancel(&sys->sched, audio_event, dsp);
    gc_sched_add(&sys->sched, sys->cpu.cycles + audio_period(dsp), "audio", audio_event, dsp);
}

void gc_dsp_init(gc_dsp_t *dsp, struct gc_system *sys)
{
    dsp->sys = sys;
    gc_mmio_register(&sys->mem.io, "DSP", GC_MMIO_DSP, 0x100, dsp_read, dsp_write, dsp);
}
