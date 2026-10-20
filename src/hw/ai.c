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
#include "hw/ai.h"
#include "hw/pi.h"

/* Frecuencias como divisores de 108 MHz (Mixer.h de Dolphin): 48 043 y 32 028 Hz */
#define RATE_DIVIDEND 108000000u
#define DIV_48K       2248u
#define DIV_32K       3372u

static void update_interrupts(gc_ai_t *ai)
{
    gc_pi_set_interrupt(&ai->sys->pi, PI_INT_AI,
                        (ai->control & AI_CR_AIINT) && (ai->control & AI_CR_AIINTMSK));
}

static int playing(const gc_ai_t *ai)
{
    return (ai->control & AI_CR_PSTAT) != 0;
}

static uint64_t now(const gc_ai_t *ai)
{
    return ai->sys->cpu.cycles;
}

static void set_ais_rate(gc_ai_t *ai, int k48)
{
    if (k48)
        ai->control |= AI_CR_AISFR;
    else
        ai->control &= ~AI_CR_AISFR;
    ai->cycles_per_sample = (uint64_t)GC_CPU_CLOCK * (k48 ? DIV_48K : DIV_32K) / RATE_DIVIDEND;
}

/* Suma muestras y prende AIINT si el contador pasó por AIIT (IncreaseSampleCount) */
static void add_samples(gc_ai_t *ai, uint32_t n)
{
    if (!playing(ai))
        return;
    const uint32_t old = ai->counter + 1;
    ai->counter += n;
    if (ai->timing - old <= ai->counter - old) {
        ai->control |= AI_CR_AIINT;
        update_interrupts(ai);
    }
}

/* Ciclos hasta el próximo vistazo: hasta AIIT, como mucho un segundo */
static uint64_t period(const gc_ai_t *ai)
{
    const uint64_t to_it = ai->cycles_per_sample * (uint32_t)(ai->timing - ai->counter);
    const uint64_t second = ai->cycles_per_sample * (RATE_DIVIDEND / DIV_48K);
    if (to_it == 0)
        return second;
    return to_it < second ? to_it : second;
}

static void catch_up(gc_ai_t *ai)
{
    const uint64_t diff = now(ai) - ai->last;
    if (diff > ai->cycles_per_sample) {
        const uint32_t n = (uint32_t)(diff / ai->cycles_per_sample);
        ai->last += (uint64_t)n * ai->cycles_per_sample;
        add_samples(ai, n);
    }
}

static void ai_event(void *ctx, uint64_t when, uint64_t late)
{
    gc_ai_t *ai = ctx;
    (void)late;
    if (!playing(ai))
        return;
    catch_up(ai);
    gc_sched_add(&ai->sys->sched, when + period(ai), "ai", ai_event, ai);
}

static void reschedule(gc_ai_t *ai)
{
    gc_sched_cancel(&ai->sys->sched, ai_event, ai);
    gc_sched_add(&ai->sys->sched, now(ai) + period(ai), "ai", ai_event, ai);
}

uint32_t gc_ai_sample_counter(const gc_ai_t *ai)
{
    /* Parado, el contador queda quieto. Dolphin suma acá el ciclo en que paró,
     * un error que no se copia. */
    if (!playing(ai))
        return ai->counter;
    return ai->counter + (uint32_t)((now(ai) - ai->last) / ai->cycles_per_sample);
}

static void write_control(gc_ai_t *ai, uint32_t v)
{
    ai->control = (ai->control & ~(AI_CR_AIINTMSK | AI_CR_AIINTVLD)) |
                  (v & (AI_CR_AIINTMSK | AI_CR_AIINTVLD));
    if ((v ^ ai->control) & AI_CR_AISFR)
        set_ais_rate(ai, (v & AI_CR_AISFR) != 0);
    if ((v ^ ai->control) & AI_CR_AIDFR)
        ai->control ^= AI_CR_AIDFR;                    /* el DMA de audio lo lee del DSP */
    if ((v ^ ai->control) & AI_CR_PSTAT) {
        if (playing(ai))                               /* al parar, lo ya tocado se cuenta */
            ai->counter = gc_ai_sample_counter(ai);
        ai->control ^= AI_CR_PSTAT;
        ai->last = now(ai);
        reschedule(ai);
    }
    if (v & AI_CR_AIINT)
        ai->control &= ~AI_CR_AIINT;
    if (v & AI_CR_SCRESET) {
        ai->counter = 0;
        ai->last = now(ai);
    }
    update_interrupts(ai);
}

static uint32_t ai_read(void *ctx, uint32_t off, int size)
{
    gc_ai_t *ai = ctx;
    if (size != 4) {
        GC_LOGW("ai: lectura de %d bits en 0x%02x", size * 8, (unsigned)off);
        return 0;
    }
    switch (off) {
    case AI_CR:   return ai->control;
    case AI_VR:   return ai->volume;
    case AI_SCNT: return gc_ai_sample_counter(ai);
    case AI_IT:   return ai->timing;
    default:      return 0;
    }
}

static void ai_write(void *ctx, uint32_t off, uint32_t v, int size)
{
    gc_ai_t *ai = ctx;
    if (size != 4) {
        GC_LOGW("ai: escritura de %d bits en 0x%02x ignorada", size * 8, (unsigned)off);
        return;
    }
    switch (off) {
    case AI_CR:
        write_control(ai, v);
        break;
    case AI_VR:
        ai->volume = v;
        break;
    case AI_SCNT:
        ai->counter = v;
        ai->last = now(ai);
        reschedule(ai);
        break;
    case AI_IT:
        ai->timing = v;
        reschedule(ai);
        break;
    default:
        GC_LOGW("ai: escritura a 0x%02x ignorada", (unsigned)off);
        break;
    }
}

void gc_ai_reset(gc_ai_t *ai)
{
    struct gc_system *sys = ai->sys;
    memset(ai, 0, sizeof *ai);
    ai->sys = sys;
    set_ais_rate(ai, 1);                               /* AIS a 48 kHz */
    ai->control |= AI_CR_AIDFR;                        /* AID a 32 kHz */
    gc_sched_cancel(&sys->sched, ai_event, ai);
    update_interrupts(ai);
}

void gc_ai_init(gc_ai_t *ai, struct gc_system *sys)
{
    ai->sys = sys;
    gc_mmio_register(&sys->mem.io, "AI", GC_MMIO_AI, 0x100, ai_read, ai_write, ai);
}
