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
 * El estado de GX en texto, para compararlo con el de Dolphin (la traza de la
 * CPU no ve BP ni XF). El parche de Dolphin (tools/dolphin-trace/traza.patch)
 * escribe exactamente lo mismo desde bpmem, FillCPMemoryArray, xfmem, el
 * SCPFifoStruct del CP y los punteros del FIFO del PI:
 *
 *   # gc-ps3 estado gx v1
 *   fifo base 00100000        (y end, hi, lo, distancia, escritura, lectura, bp)
 *   fifo banderas 00000111    (ver abajo)
 *   pi base 00100000          (y end, escritura)
 *   gp 0                      bytes juntados en el gather pipe
 *   bp 41 000000aa            las palabras distintas de cero
 *   cp 50 00000600
 *   xf 1008 00000011
 */
#include <stdio.h>

#include "core/system.h"
#include "gx/gx.h"

/* fifo banderas: un bit por cada una, en este orden */
static uint32_t fifo_flags(const gc_cp_fifo_t *f)
{
    return (uint32_t)f->read_enable | (uint32_t)f->bp_enable << 1 | (uint32_t)f->bp_int << 2 |
           (uint32_t)f->breakpoint << 3 | (uint32_t)f->lo_int << 4 | (uint32_t)f->hi_int << 5 |
           (uint32_t)f->lo << 6 | (uint32_t)f->hi << 7 | (uint32_t)f->link << 8;
}

void gc_gx_estado(const struct gc_system *s, void (*line)(void *ctx, const char *text), void *ctx)
{
    char buf[64];
    const gc_cp_fifo_t *f = &s->cp.fifo;
    const gc_gx_t *gx = &s->gx;

    snprintf(buf, sizeof buf, "# gc-ps3 estado gx v%d", GC_GX_ESTADO_VERSION);
    line(ctx, buf);

    const struct { const char *name; uint32_t v; } regs[] = {
        { "fifo base", f->base }, { "fifo end", f->end }, { "fifo hi", f->hi_wm },
        { "fifo lo", f->lo_wm }, { "fifo distancia", f->distance },
        { "fifo escritura", f->wptr }, { "fifo lectura", f->rptr }, { "fifo bp", f->bp },
        { "fifo banderas", fifo_flags(f) }, { "pi base", s->pi.fifo_base },
        { "pi end", s->pi.fifo_end }, { "pi escritura", s->pi.fifo_wptr },
    };
    for (size_t k = 0; k < sizeof regs / sizeof regs[0]; k++) {
        snprintf(buf, sizeof buf, "%s %08x", regs[k].name, (unsigned)regs[k].v);
        line(ctx, buf);
    }
    snprintf(buf, sizeof buf, "gp %u", (unsigned)s->gpfifo.count);
    line(ctx, buf);

    for (int k = 0; k < 256; k++)
        if (gx->bp[k]) {
            snprintf(buf, sizeof buf, "bp %02x %08x", (unsigned)k, (unsigned)gx->bp[k]);
            line(ctx, buf);
        }
    uint32_t cp[256];
    gc_gx_cp_memory(gx, cp);
    for (int k = 0; k < 256; k++)
        if (cp[k]) {
            snprintf(buf, sizeof buf, "cp %02x %08x", (unsigned)k, (unsigned)cp[k]);
            line(ctx, buf);
        }
    for (uint32_t k = 0; k < GC_XF_SIZE; k++)
        if (gx->xf[k]) {
            snprintf(buf, sizeof buf, "xf %04x %08x", (unsigned)k, (unsigned)gx->xf[k]);
            line(ctx, buf);
        }
}
