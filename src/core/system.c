/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
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
 * Valores del arranque: Dolphin (commit cf5cb7ef), Source/Core/Core/Boot/Boot.cpp
 * (arranque de un ejecutable) y Boot_BS2Emu.cpp (SetupMSR, SetupHID, SetupBAT,
 * SetupGCMemory). Tabla completa en docs/arranque-hle.md.
 */
#include <string.h>

#include "core/log.h"
#include "core/system.h"

#define RFI 0x4C000064u

/* Vectores de excepción que el IPL deja con un rfi (CopyDefaultExceptionHandlers) */
static const uint32_t vectors[] = {
    0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800,
    0x0900, 0x0C00, 0x0D00, 0x0F00, 0x1300, 0x1400, 0x1700,
};

/* Memoria baja (YAGCD 4.2.1) */
#define LOW_BOOT_MAGIC    0x80000020u      /* 0x0D15EA5E: arrancó desde el IPL */
#define LOW_MEM_SIZE      0x80000028u
#define LOW_CONSOLE_TYPE  0x8000002Cu
#define LOW_VIDEO_MODE    0x800000CCu      /* 0 NTSC, 1 PAL */
#define LOW_ARAM_SIZE     0x800000D0u
#define LOW_BUS_CLOCK     0x800000F8u
#define LOW_CPU_CLOCK     0x800000FCu
#define LOW_SYSTEM_TIME   0x800030D8u      /* u64: ticks de timebase desde 2000 */

#define BOOT_MAGIC        0x0D15EA5Eu
#define CONSOLE_DEVKIT    0x10000006u      /* el último devkit, como Dolphin (D-032) */

int gc_system_init(gc_system_t *s)
{
    memset(s, 0, sizeof *s);
    if (gc_mem_init(&s->mem) != 0)
        return -1;
    gekko_reset(&s->cpu, &s->mem);
    s->cpu.hle = gc_hle_dispatch;
    s->cpu.hle_ctx = s;
    gc_sched_init(&s->sched);
    gc_hle_reset(&s->hle);
    gc_pi_init(&s->pi, s);
    gc_vi_init(&s->vi, s);
    gc_vi_reset(&s->vi, 0);
    return 0;
}

void gc_system_free(gc_system_t *s)
{
    gc_mem_free(&s->mem);
}

void gc_system_hle_boot(gc_system_t *s, const gc_boot_config_t *cfg, uint32_t entry)
{
    gekko_t *c = &s->cpu;
    gc_mem_t *m = &s->mem;

    memset(m->ram, 0, GC_RAM_SIZE);
    gekko_reset(c, m);
    c->hle = gc_hle_dispatch;
    c->hle_ctx = s;
    gc_sched_init(&s->sched);
    gc_hle_reset(&s->hle);
    gc_pi_reset(&s->pi);
    gc_vi_reset(&s->vi, cfg && cfg->pal);

    c->msr = MSR_FP | MSR_IR | MSR_DR | MSR_RI;           /* 0x2032 */
    c->spr[SPR_HID0] = 0x0011C464u;                        /* cachés, BHT, BTIC... */
    c->spr[SPR_HID2] = HID2_LSQE | HID2_WPE | HID2_PSE;    /* 0xE0000000 */
    c->spr[SPR_IBAT0U] = 0x80001FFFu;                      /* 0x80000000: 256 MB con caché */
    c->spr[SPR_IBAT0L] = 0x00000002u;
    c->spr[SPR_DBAT0U] = 0x80001FFFu;
    c->spr[SPR_DBAT0L] = 0x00000002u;
    c->spr[SPR_DBAT1U] = 0xC0001FFFu;                      /* 0xC0000000: sin caché */
    c->spr[SPR_DBAT1L] = 0x0000002Au;

    for (size_t k = 0; k < sizeof vectors / sizeof vectors[0]; k++)
        gc_write32(m, 0x80000000u | vectors[k], RFI);

    gc_write32(m, LOW_BOOT_MAGIC, BOOT_MAGIC);
    gc_write32(m, LOW_MEM_SIZE, GC_RAM_SIZE);
    gc_write32(m, LOW_CONSOLE_TYPE, CONSOLE_DEVKIT);
    gc_write32(m, LOW_VIDEO_MODE, cfg && cfg->pal ? 1u : 0u);
    gc_write32(m, LOW_ARAM_SIZE, GC_ARAM_SIZE);
    gc_write32(m, LOW_BUS_CLOCK, GC_BUS_CLOCK);
    gc_write32(m, LOW_CPU_CLOCK, GC_CPU_CLOCK);
    gc_write64(m, LOW_SYSTEM_TIME, (cfg ? cfg->rtc : 0) * (uint64_t)GC_TB_CLOCK);

    /* DEC con el bit 0 ya en 1: no hay un decrementador pendiente del arranque
     * (como en Dolphin, que no lo dispara hasta que el programa escribe DEC). */
    c->dec = 0xFFFFFFFFu;

    c->pc = entry;
    gekko_fpu_sync(c);
}

gc_dol_error_t gc_system_boot_dol(gc_system_t *s, const uint8_t *data, size_t len,
                                  const gc_boot_config_t *cfg)
{
    gc_dol_t dol;
    int bad;
    const gc_dol_error_t err = gc_dol_parse(data, len, &dol, &bad);
    if (err != GC_DOL_OK) {
        if (bad >= 0)
            GC_LOGE("dol: %s (seccion %d)", gc_dol_strerror(err), bad);
        else
            GC_LOGE("dol: %s", gc_dol_strerror(err));
        return err;
    }
    gc_system_hle_boot(s, cfg, dol.entry);
    gc_dol_load(&dol, data, len, &s->mem);
    GC_LOGI("dol: cargado, entrada 0x%08x, BSS 0x%08x (%u bytes)", (unsigned)dol.entry,
            (unsigned)dol.bss_addr, (unsigned)dol.bss_size);
    return GC_DOL_OK;
}

int gc_system_load_map(gc_system_t *s, const char *text, size_t len)
{
    return gc_hle_load_map(s, text, len);
}

uint64_t gc_system_run(gc_system_t *s, uint64_t cycles)
{
    gekko_t *c = &s->cpu;
    const uint64_t start = c->cycles, end = start + cycles;

    while (c->cycles < end && !c->stop) {
        gc_sched_dispatch(&s->sched, c->cycles);
        if (c->stop)
            break;
        uint64_t until = gc_sched_next(&s->sched);
        if (until > end)
            until = end;
        if (until <= c->cycles)              /* evento que no se va: avanza igual */
            until = c->cycles + 1;
        gekko_run(c, until - c->cycles);
    }
    return c->cycles - start;
}
