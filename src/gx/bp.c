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
 * Registros de BP: VideoCommon/BPStructs.cpp de Dolphin (LoadBPReg y
 * BPWritten). El valor nuevo pasa por la máscara de BP (0xFE), que vuelve a
 * 0xFFFFFF después de cualquier otra escritura. En esta etapa solo hacen algo
 * el token (0x47, 0x48) y "draw done" (0x45 = 2); el resto se guarda, y los
 * registros que disparan algo (copia del EFB, cargas de TLUT y de TMEM...) se
 * avisan una vez en el log hasta la etapa C.
 */
#include "core/log.h"
#include "core/system.h"
#include "gx/gx.h"

/* Los que tienen efecto aunque se escriba el mismo valor (BPWritten) */
static int always_acts(uint8_t reg)
{
    switch (reg) {
    case 0x52:                     /* BPMEM_TRIGGER_EFB_COPY */
    case 0x55: case 0x56:          /* BPMEM_CLEARBBOX1/2 */
    case 0x57:                     /* BPMEM_CLEAR_PIXEL_PERF */
    case GX_BP_DRAWDONE: case GX_BP_TOKEN: case GX_BP_TOKEN_INT:
    case 0x63:                     /* BPMEM_PRELOAD_MODE */
    case 0x64: case 0x65:          /* BPMEM_LOADTLUT0/1 */
    case 0x66:                     /* BPMEM_TEXINVALIDATE */
        return 1;
    default:
        return 0;
    }
}

static void note_pending(gc_gx_t *gx, uint8_t reg, uint32_t v)
{
    if (gx->bp_noted[reg])
        return;
    gx->bp_noted[reg] = 1;
    GC_LOGI("gx: BP 0x%02x = 0x%06x guardado; su efecto llega en la etapa C", (unsigned)reg,
            (unsigned)v);
}

void gc_gx_load_bp(gc_gx_t *gx, uint8_t reg, uint32_t value)
{
    const uint32_t mask = gx->bp[GX_BP_MASK];
    const uint32_t old = gx->bp[reg];
    const uint32_t nv = (old & ~mask) | (value & mask);

    if (reg != GX_BP_MASK)
        gx->bp[GX_BP_MASK] = 0xFFFFFFu;

    if (old == nv && !always_acts(reg))
        return;
    gx->bp[reg] = nv;

    gc_pe_t *pe = &gx->sys->pe;
    switch (reg) {
    case GX_BP_DRAWDONE:
        if ((nv & 0xFFu) == 0x02)
            gc_pe_set_finish(pe, gx->cycles);
        return;
    case GX_BP_TOKEN:
        gc_pe_set_token(pe, (uint16_t)nv, 0, gx->cycles);
        return;
    case GX_BP_TOKEN_INT:
        gc_pe_set_token(pe, (uint16_t)nv, 1, gx->cycles);
        return;
    default:
        if (always_acts(reg))
            note_pending(gx, reg, nv);
        return;
    }
}
