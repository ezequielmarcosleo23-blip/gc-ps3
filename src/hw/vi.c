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
#include "hw/vi.h"

#define VI_CLOCK_27 27000000u
#define VI_CLOCK_54 54000000u
#define MIN_HALF_LINE 64u           /* si HLW vale 0, el evento no puede repetirse en el mismo ciclo */
#define SI_POLL_DELAY 15u           /* medias líneas que tarda un sondeo de mandos */

static uint16_t r16(const gc_vi_t *vi, uint32_t off) { return vi->reg[(off & (VI_REGS_SIZE - 1)) >> 1]; }
static void w16(gc_vi_t *vi, uint32_t off, uint16_t v) { vi->reg[(off & (VI_REGS_SIZE - 1)) >> 1] = v; }
static uint32_t r32(const gc_vi_t *vi, uint32_t off) { return (uint32_t)r16(vi, off) << 16 | r16(vi, off + 2); }

static void w32(gc_vi_t *vi, uint32_t off, uint32_t v)
{
    w16(vi, off, (uint16_t)(v >> 16));
    w16(vi, off + 2, (uint16_t)v);
}

/* ---- campos de los registros -------------------------------------------------- */

static uint32_t equ(const gc_vi_t *vi) { return r16(vi, VI_VTR) & 0xFu; }
static uint32_t acv(const gc_vi_t *vi) { return (r16(vi, VI_VTR) >> 4) & 0x3FFu; }
static uint32_t hlw(const gc_vi_t *vi) { return r32(vi, VI_HTR0) & 0x3FFu; }
static uint32_t prb(const gc_vi_t *vi, uint32_t reg) { return r32(vi, reg) & 0x3FFu; }
static uint32_t psb(const gc_vi_t *vi, uint32_t reg) { return (r32(vi, reg) >> 16) & 0x3FFu; }

uint32_t gc_vi_half_lines_odd(const gc_vi_t *vi)
{
    return 3 * equ(vi) + prb(vi, VI_VTO) + 2 * acv(vi) + psb(vi, VI_VTO);
}

uint32_t gc_vi_half_lines_even(const gc_vi_t *vi)
{
    return 3 * equ(vi) + prb(vi, VI_VTE) + 2 * acv(vi) + psb(vi, VI_VTE);
}

uint32_t gc_vi_ticks_per_half_line(const gc_vi_t *vi)
{
    const uint32_t clock = (r16(vi, VI_CLOCK) & 1u) ? VI_CLOCK_54 : VI_CLOCK_27;
    const uint32_t per_sample = 2u * GC_CPU_CLOCK / clock;          /* 36 o 18 */
    const uint32_t t = per_sample * hlw(vi);
    return t < MIN_HALF_LINE ? MIN_HALF_LINE : t;
}

/* XFB de arriba y de abajo: con POFF, la dirección está en bloques de 32 bytes
 * (el POFF de abajo es el mismo de arriba). */
static uint32_t xfb_address(const gc_vi_t *vi, uint32_t reg)
{
    const uint32_t fbb = r32(vi, reg) & 0x00FFFFFFu;
    return (r32(vi, VI_TFBL) & 0x10000000u) ? fbb << 5 : fbb;
}

/* Primera y última media línea activa de cada campo (UpdateParameters de Dolphin). */
static void update_parameters(gc_vi_t *vi)
{
    const uint32_t equ_hl = 3 * equ(vi), acv_hl = 2 * acv(vi);
    vi->odd_first = equ_hl + prb(vi, VI_VTO);
    const uint32_t odd_end = vi->odd_first + acv_hl;
    vi->odd_last = odd_end - 1;
    /* El campo par se acomoda para que los dos campos duren lo mismo */
    const int32_t psb_diff = (int32_t)psb(vi, VI_VTO) - (int32_t)psb(vi, VI_VTE);
    vi->even_first = (uint32_t)((int32_t)(odd_end + psb(vi, VI_VTO) + equ_hl + prb(vi, VI_VTE)) - psb_diff);
    vi->even_last = vi->even_first + acv_hl - 1;
}

static void update_interrupts(gc_vi_t *vi)
{
    int active = 0;
    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t di = r32(vi, VI_DI0 + 4 * k);
        if ((di & VI_DI_INT) && (di & VI_DI_MASK))
            active = 1;
    }
    gc_pi_set_interrupt(&vi->sys->pi, PI_INT_VI, active);
}

/* ---- media línea --------------------------------------------------------------- */

static void end_field(gc_vi_t *vi, int even)
{
    const uint32_t hsw = r16(vi, VI_HSW);
    gc_vi_field_t f;
    f.xfb = xfb_address(vi, even ? VI_BFBL : VI_TFBL);
    f.width = ((hsw >> 8) & 0x7Fu) * 16;               /* WPL */
    f.stride = (hsw & 0xFFu) * 16 * 2;                 /* STD, en bytes */
    f.height = acv(vi);
    f.even = even;
    vi->fields++;
    if (f.xfb) {
        vi->last = f;
        if (vi->on_field)
            vi->on_field(vi->on_field_ctx, &f);
    }
}

static void half_line(gc_vi_t *vi, uint64_t when)
{
    /* La misma cadena que Dolphin: el inicio de un campo tapa el fin del otro */
    const uint32_t hl = vi->half_line;
    if (hl == vi->odd_first || hl == vi->even_first)
        ;                                          /* inicio del campo: nada que hacer */
    else if (hl == vi->odd_last)
        end_field(vi, 0);
    else if (hl == vi->even_last)
        end_field(vi, 1);

    /* Sondeo de los mandos (Update de Dolphin): el que toca ahora y, en el
     * comienzo de cada campo, el primero del campo */
    const int field_start = hl == 0 || hl == gc_vi_half_lines_odd(vi);
    if (hl == vi->next_si_poll) {
        gc_si_poll(&vi->sys->si);
        vi->next_si_poll += 2 * gc_si_poll_lines(&vi->sys->si);
    }
    if (field_start)
        vi->next_si_poll = hl + SI_POLL_DELAY;

    const uint32_t total = gc_vi_half_lines_odd(vi) + gc_vi_half_lines_even(vi);
    vi->half_line++;
    if (vi->half_line >= total)
        vi->half_line = 0;
    if (!(vi->half_line & 1u))
        vi->line_start = when;

    /* DIn se activa al llegar a su línea VCT, en la primera o segunda mitad
     * según HCT (la posición horizontal solo se resuelve por medias líneas). */
    const uint32_t line = 1 + vi->half_line / 2;
    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t off = VI_DI0 + 4 * k, di = r32(vi, off);
        const uint32_t hct = di & 0x7FFu, vct = (di >> 16) & 0x7FFu;
        const uint32_t second_half = hct > hlw(vi) ? 1u : 0u;
        if (line == vct && (vi->half_line & 1u) == second_half)
            w32(vi, off, di | VI_DI_INT);
    }
    update_interrupts(vi);
}

static void half_line_event(void *ctx, uint64_t when, uint64_t late)
{
    gc_vi_t *vi = ctx;
    (void)late;
    half_line(vi, when);
    gc_sched_add(&vi->sys->sched, when + gc_vi_ticks_per_half_line(vi), "vi", half_line_event, vi);
}

/* ---- registros ---------------------------------------------------------------------- */

static uint16_t read_reg(gc_vi_t *vi, uint32_t off)
{
    switch (off) {
    case VI_DPV:
        return (uint16_t)(1 + vi->half_line / 2);
    case VI_DPH: {
        const uint64_t now = vi->sys->cpu.cycles;
        const uint32_t w = hlw(vi);
        uint64_t v = 1 + (uint64_t)w * (now - vi->line_start) / gc_vi_ticks_per_half_line(vi);
        if (v > 2u * w)
            v = 2u * w;
        return (uint16_t)(v < 1 ? 1 : v);
    }
    default:
        return r16(vi, off);
    }
}

static void write_reg(gc_vi_t *vi, uint32_t off, uint16_t v)
{
    switch (off) {
    case VI_DCR:
        w16(vi, off, v & ~0x0002u);                    /* RST no queda escrito */
        if (v & 0x0002u) {                             /* reset: interrupciones a cero */
            for (uint32_t k = 0; k < 4; k++)
                w32(vi, VI_DI0 + 4 * k, 0);
            update_interrupts(vi);
        }
        update_parameters(vi);
        break;
    case VI_VTR:
    case VI_HTR0: case VI_HTR0 + 2:
    case VI_VTO: case VI_VTO + 2:
    case VI_VTE: case VI_VTE + 2:
    case VI_CLOCK:
        w16(vi, off, v);
        update_parameters(vi);
        break;
    case VI_TFBL: case VI_TFBR: case VI_BFBL: case VI_BFBR:
        w16(vi, off, v);
        if (v & 0xE000u)                               /* CLRPOFF borra POFF */
            w16(vi, off, (uint16_t)(v & ~0x1000u));
        break;
    case VI_DI0: case VI_DI1: case VI_DI2: case VI_DI3:
        w16(vi, off, v);
        update_interrupts(vi);
        break;
    case VI_DPV:
    case VI_DPH:
        GC_LOGW("vi: escritura a la posicion del haz (0x%02x) ignorada", (unsigned)off);
        break;
    default:
        w16(vi, off, v);
        break;
    }
}

static uint32_t vi_read(void *ctx, uint32_t off, int size)
{
    gc_vi_t *vi = ctx;
    if (off >= VI_REGS_SIZE)                           /* fuera de los registros */
        return 0;
    if (size == 4)
        return (uint32_t)read_reg(vi, off & ~3u) << 16 | read_reg(vi, (off & ~3u) + 2);
    const uint16_t v = read_reg(vi, off & ~1u);
    if (size == 2)
        return v;
    return (off & 1) ? (v & 0xFFu) : (uint32_t)(v >> 8);
}

static void vi_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_vi_t *vi = ctx;
    if (off >= VI_REGS_SIZE) {
        GC_LOGW("vi: escritura a 0x%02x fuera de los registros ignorada", (unsigned)off);
        return;
    }
    if (size == 4) {
        write_reg(vi, off & ~3u, (uint16_t)(v >> 16));
        write_reg(vi, (off & ~3u) + 2, (uint16_t)v);
    } else if (size == 2) {
        write_reg(vi, off & ~1u, (uint16_t)v);
    } else {
        GC_LOGW("vi: escritura de 8 bits a 0x%02x ignorada", (unsigned)off);
    }
}

void gc_vi_init(gc_vi_t *vi, struct gc_system *sys)
{
    memset(vi, 0, sizeof *vi);
    vi->sys = sys;
    gc_mmio_register(&sys->mem.io, "VI", GC_MMIO_VI, 0x100, vi_read, vi_write, vi);
}

void gc_vi_reset(gc_vi_t *vi, int pal)
{
    memset(vi->reg, 0, sizeof vi->reg);
    /* Valores de encendido de Dolphin (VideoInterfaceManager::Preset) */
    w16(vi, VI_VTR, 0x0006);                           /* EQU = 6, ACV = 0 */
    w16(vi, VI_DCR, (uint16_t)(0x0001 | (pal ? 0x0100 : 0)));   /* ENB, FMT */
    w32(vi, VI_HTR0, 0x476901ADu);                     /* HLW 429, HCE 105, HCS 71 */
    w32(vi, VI_HTR1, 0x02EA5140u);                     /* HSY 64, HBE 162, HBS 373 */
    w32(vi, VI_VTO, 0x000501F6u);                      /* PRB 502, PSB 5 */
    w32(vi, VI_VTE, 0x000401F7u);                      /* PRB 503, PSB 4 */
    w32(vi, VI_BBOI, 0x410C410Cu);
    w32(vi, VI_BBEI, 0x40ED40EDu);
    w32(vi, VI_DI0, 0x110701AEu);                      /* VCT 263, HCT 430, habilitada */
    w32(vi, VI_DI1, 0x10010001u);                      /* VCT 1, HCT 1, habilitada */
    w16(vi, VI_HSW, 0x2828);                           /* WPL 40, STD 40 */
    w16(vi, VI_CLOCK, pal ? 0 : 1);                    /* como Dolphin: 54 MHz en NTSC */
    vi->half_line = 0;
    vi->line_start = vi->sys->cpu.cycles;
    vi->fields = 0;
    vi->next_si_poll = SI_POLL_DELAY;
    memset(&vi->last, 0, sizeof vi->last);
    update_parameters(vi);
    /* Como Dolphin, sin tocar la causa VI del PI: sigue prendida (valor de
     * encendido del PI) hasta la primera media línea. */
    gc_sched_cancel(&vi->sys->sched, half_line_event, vi);
    gc_sched_add(&vi->sys->sched, vi->sys->cpu.cycles + gc_vi_ticks_per_half_line(vi), "vi",
                 half_line_event, vi);
}
