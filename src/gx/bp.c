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
 * 0xFFFFFF después de cualquier otra escritura. Hacen algo el token (0x47,
 * 0x48), "draw done" (0x45 = 2), la copia del EFB (0x52, copy.c), la tijera
 * (0x20, 0x21, 0x59), la caja envolvente (0x55, 0x56), las cargas de la TMEM
 * (0x63, 0x65) y los colores del TEV (0xE0-0xE7); el resto se guarda y se lee
 * al dibujar.
 */
#include "core/log.h"
#include "core/system.h"
#include "gx/gx.h"
#include "gx/raster.h"

/* Los que tienen efecto aunque se escriba el mismo valor (BPWritten) */
static int always_acts(uint8_t reg)
{
    switch (reg) {
    case GX_BP_COPY:               /* BPMEM_TRIGGER_EFB_COPY */
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
    if (reg >= 0x30 && reg <= 0x3F)
        gx->xf_dirty = 1;                       /* SU_SSIZE y SU_TSIZE: la escala de las coordenadas */

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
    case GX_BP_COPY:
        gc_gx_efb_copy(gx);
        return;
    case 0x20: case 0x21: case 0x59:    /* tijera: los rectángulos se recalculan al dibujar */
        gx->raster->scissor_dirty = 1;
        return;
    case 0x55: case 0x56: {             /* BPMEM_CLEARBBOX1/2 */
        const uint32_t i = reg & 2u;
        gc_gx_bbox_set(gx, i, (uint16_t)(nv & 0x3FFu));
        gc_gx_bbox_set(gx, i + 1, (uint16_t)(nv >> 10));
        return;
    }
    case 0x57:                          /* BPMEM_CLEAR_PIXEL_PERF: sin contadores (como Dolphin) */
        return;
    case 0x63:                          /* BPMEM_PRELOAD_MODE: precarga a la TMEM */
        gc_gx_preload(gx);
        return;
    case 0x64:                          /* BPMEM_LOADTLUT0: solo la dirección */
    case 0x66:                          /* BPMEM_TEXINVALIDATE: sin caché de texturas */
        return;
    case 0x65:                          /* BPMEM_LOADTLUT1: paleta a la TMEM */
        gc_gx_load_tlut(gx);
        return;
    case 0xE0: case 0xE2: case 0xE4: case 0xE6: {   /* TEV: rojo y alfa */
        int16_t *c = (nv >> 23) & 1u ? gx->tev_kcolor[(reg >> 1) & 3u] : gx->tev_color[(reg >> 1) & 3u];
        c[0] = (int16_t)((int32_t)(nv << 21) >> 21);
        c[3] = (int16_t)((int32_t)(nv << 9) >> 21);
        return;
    }
    case 0xE1: case 0xE3: case 0xE5: case 0xE7: {   /* TEV: azul y verde */
        int16_t *c = (nv >> 23) & 1u ? gx->tev_kcolor[(reg >> 1) & 3u] : gx->tev_color[(reg >> 1) & 3u];
        c[2] = (int16_t)((int32_t)(nv << 21) >> 21);
        c[1] = (int16_t)((int32_t)(nv << 9) >> 21);
        return;
    }
    default:
        return;
    }
}
