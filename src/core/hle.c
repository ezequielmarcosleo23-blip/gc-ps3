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
#include <stdio.h>
#include <string.h>

#include "core/hle.h"
#include "core/log.h"
#include "core/system.h"

void gc_hle_reset(gc_hle_t *h)
{
    const gc_report_fn report = h->report;
    void *ctx = h->report_ctx;
    memset(h, 0, sizeof *h);
    h->report = report;
    h->report_ctx = ctx;
}

int gc_hle_patch(gc_system_t *s, uint32_t addr, const char *name, gc_hle_fn fn)
{
    gc_hle_t *h = &s->hle;
    addr &= ~3u;
    for (uint32_t k = 0; k < h->count; k++)
        if (h->hooks[k].addr == addr)
            return (int)k;
    if (h->count == GC_HLE_MAX || gc_mem_region(addr) != GC_REGION_RAM || !gc_ram_ptr(&s->mem, addr, 4))
        return -1;
    gc_hook_t *hk = &h->hooks[h->count];
    hk->addr = addr;
    hk->orig = gc_read32(&s->mem, addr);
    hk->fn = fn;
    snprintf(hk->name, sizeof hk->name, "%s", name);
    gc_write32(&s->mem, addr, GEKKO_HLE_INST(h->count));
    return (int)h->count++;
}

uint32_t gc_hle_original(const gc_system_t *s, uint32_t inst)
{
    if ((inst >> 26) != GEKKO_HLE_OPCODE)
        return inst;
    const uint32_t index = inst & 0x03FFFFFFu;
    return index < s->hle.count ? s->hle.hooks[index].orig : inst;
}

uint32_t gc_hle_dispatch(void *ctx, gekko_t *c, uint32_t index)
{
    gc_system_t *s = ctx;
    gc_hle_t *h = &s->hle;
    if (index >= h->count) {
        GC_LOGE("hle: gancho %u inexistente en 0x%08x", (unsigned)index, (unsigned)c->pc);
        return GEKKO_HLE_INST(index);               /* la CPU la toma como ilegal */
    }
    h->hooks[index].fn(s);
    return h->hooks[index].orig;
}

/* ---- salida del programa --------------------------------------------------- */

static void deliver(gc_system_t *s)
{
    gc_hle_t *h = &s->hle;
    h->line[h->line_len] = '\0';
    if (h->report)
        h->report(h->report_ctx, h->line);
    else
        GC_LOGI("os: %s", h->line);
    h->line_len = 0;
    h->lines++;
}

void gc_hle_output(gc_system_t *s, const char *text, size_t n)
{
    gc_hle_t *h = &s->hle;
    for (size_t k = 0; k < n; k++) {
        if (text[k] == '\n') {
            deliver(s);
            continue;
        }
        if (h->line_len == GC_HLE_LINE_MAX - 1)     /* línea demasiado larga: se corta */
            deliver(s);
        h->line[h->line_len++] = text[k];
    }
}

void gc_hle_flush(gc_system_t *s)
{
    if (s->hle.line_len)
        deliver(s);
}

/* ---- .map ------------------------------------------------------------------------ */

static int is_hex(const char *t, size_t n)
{
    if (n == 0)
        return 0;
    for (size_t k = 0; k < n; k++)
        if (!((t[k] >= '0' && t[k] <= '9') || (t[k] >= 'a' && t[k] <= 'f') || (t[k] >= 'A' && t[k] <= 'F')))
            return 0;
    return 1;
}

static int is_dec(const char *t, size_t n)
{
    if (n == 0)
        return 0;
    for (size_t k = 0; k < n; k++)
        if (t[k] < '0' || t[k] > '9')
            return 0;
    return 1;
}

static uint32_t hex32(const char *t, size_t n)
{
    uint32_t v = 0;
    for (size_t k = 0; k < n; k++) {
        const char ch = t[k];
        v = v << 4 | (uint32_t)(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10);
    }
    return v;
}

#define MAX_TOKENS 8

/* Parte una línea en palabras separadas por espacios o tabuladores. */
static int tokens(const char *line, size_t len, const char *tok[], size_t tlen[])
{
    int n = 0;
    size_t k = 0;
    while (k < len && n < MAX_TOKENS) {
        while (k < len && (line[k] == ' ' || line[k] == '\t' || line[k] == '\r'))
            k++;
        if (k == len)
            break;
        tok[n] = line + k;
        while (k < len && line[k] != ' ' && line[k] != '\t' && line[k] != '\r')
            k++;
        tlen[n] = (size_t)(line + k - tok[n]);
        n++;
    }
    return n;
}

static const gc_hle_known_t *known(const char *name, size_t n)
{
    for (const gc_hle_known_t *k = gc_hle_known; k->name; k++)
        if (strlen(k->name) == n && memcmp(k->name, name, n) == 0)
            return k;
    return NULL;
}

int gc_hle_load_map(gc_system_t *s, const char *text, size_t len)
{
    int hooked = 0, in_text = 0;
    size_t pos = 0;
    while (pos < len) {
        const char *line = text + pos;
        size_t n = 0;
        while (pos + n < len && line[n] != '\n')
            n++;
        pos += n + 1;

        const char *tok[MAX_TOKENS];
        size_t tl[MAX_TOKENS];
        const int nt = tokens(line, n, tok, tl);
        if (nt == 0)
            continue;
        /* "<sección> section layout" (CodeWarrior y Dolphin) o ".text" sola */
        if ((nt >= 3 && tl[1] == 7 && memcmp(tok[1], "section", 7) == 0 && tl[2] == 6 &&
             memcmp(tok[2], "layout", 6) == 0) || (nt == 1 && tok[0][0] == '.')) {
            in_text = (tl[0] == 5 && memcmp(tok[0], ".text", 5) == 0) ||
                      (tl[0] == 5 && memcmp(tok[0], ".init", 5) == 0);
            continue;
        }
        if (!in_text || tl[0] != 8 || !is_hex(tok[0], 8))
            continue;

        /* 2 columnas: dirección nombre. 3 o 4: inicio tamaño virtual [archivo]
         * [alineación] nombre. */
        uint32_t vaddr;
        int t = 1;
        if (nt >= 4 && is_hex(tok[1], tl[1]) && tl[2] == 8 && is_hex(tok[2], 8)) {
            vaddr = hex32(tok[2], 8);
            t = 3;
            if (t < nt - 1 && tl[t] == 8 && is_hex(tok[t], 8))
                t++;                                /* posición en el archivo */
            if (t < nt - 1 && is_dec(tok[t], tl[t]))
                t++;                                /* alineación */
        } else {
            vaddr = hex32(tok[0], 8);
        }
        if (t >= nt)
            continue;
        const gc_hle_known_t *k = known(tok[t], tl[t]);
        if (k && gc_hle_patch(s, vaddr, k->name, k->fn) >= 0)
            hooked++;
    }
    GC_LOGI("hle: %d funciones enganchadas desde el .map", hooked);
    return hooked;
}
