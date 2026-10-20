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

/* Cada conversión se arma como un formato del anfitrión y la hace snprintf:
 * así %e, %g y el redondeo salen igual que en un printf de C normal. */
#include <stdio.h>
#include <string.h>

#include "core/vaformat.h"

#define GPR_FIRST 3u
#define GPR_LAST  10u
#define FPR_FIRST 1u
#define FPR_LAST  8u

void gc_va_from_regs(gc_va_t *va, gc_mem_t *mem, const gekko_t *c, uint32_t first_gpr)
{
    memset(va, 0, sizeof *va);
    va->mem = mem;
    va->cpu = c;
    va->gpr = first_gpr;
    va->fpr = FPR_FIRST;
    va->stack = c->gpr[1] + 8;
}

void gc_va_from_list(gc_va_t *va, gc_mem_t *mem, const gekko_t *c, uint32_t addr)
{
    memset(va, 0, sizeof *va);
    va->mem = mem;
    va->cpu = c;
    va->gpr = GPR_FIRST + gc_read8(mem, addr);
    va->fpr = FPR_FIRST + gc_read8(mem, addr + 1);
    va->stack = gc_read32(mem, addr + 4);
    va->gpr_area = gc_read32(mem, addr + 8);
    va->fpr_area = va->gpr_area + 4 * 8;
}

static uint32_t read_gpr(gc_va_t *va, uint32_t r)
{
    if (va->gpr_area)
        return gc_read32(va->mem, va->gpr_area + 4 * (r - GPR_FIRST));
    return va->cpu->gpr[r];
}

static uint64_t read_fpr(gc_va_t *va, uint32_t r)
{
    if (va->gpr_area)
        return gc_read64(va->mem, va->fpr_area + 8 * (r - FPR_FIRST));
    return va->cpu->fpr[r][0];
}

uint32_t gc_va_u32(gc_va_t *va)
{
    if (va->gpr <= GPR_LAST)
        return read_gpr(va, va->gpr++);
    va->stack = (va->stack + 3u) & ~3u;
    const uint32_t v = gc_read32(va->mem, va->stack);
    va->stack += 4;
    return v;
}

uint64_t gc_va_u64(gc_va_t *va)
{
    if (va->gpr <= GPR_LAST && (va->gpr & 1u) == 0)
        va->gpr++;                          /* los pares empiezan en un registro impar */
    if (va->gpr < GPR_LAST) {
        const uint64_t v = (uint64_t)read_gpr(va, va->gpr) << 32 | read_gpr(va, va->gpr + 1);
        va->gpr += 2;
        return v;
    }
    /* No entró (gpr ya es 11): este y los que siguen van por la pila. */
    va->stack = (va->stack + 7u) & ~7u;
    const uint64_t v = gc_read64(va->mem, va->stack);
    va->stack += 8;
    return v;
}

double gc_va_double(gc_va_t *va)
{
    uint64_t bits;
    if (va->fpr <= FPR_LAST) {
        bits = read_fpr(va, va->fpr++);
    } else {
        va->stack = (va->stack + 7u) & ~7u;
        bits = gc_read64(va->mem, va->stack);
        va->stack += 8;
    }
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

size_t gc_read_cstr(gc_mem_t *mem, uint32_t addr, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0)
        return 0;
    while (n + 1 < cap) {
        const char ch = (char)gc_read8(mem, addr + (uint32_t)n);
        if (!ch)
            break;
        out[n++] = ch;
    }
    out[n] = '\0';
    return n;
}

/* ---- formateo ------------------------------------------------------------------ */

typedef struct outbuf {
    char  *p;
    size_t cap, len;
} outbuf_t;

static void put(outbuf_t *o, const char *s, size_t n)
{
    for (size_t k = 0; k < n && o->len + 1 < o->cap; k++)
        o->p[o->len++] = s[k];
    o->p[o->len] = '\0';
}

#define FMT_MAX  1024     /* largo máximo del formato */
#define STR_MAX  1024     /* largo máximo de un %s */
#define SPEC_MAX 32

size_t gc_vaformat(gc_mem_t *mem, uint32_t fmt_addr, gc_va_t *va, char *out, size_t cap)
{
    static char fmt[FMT_MAX];
    char spec[SPEC_MAX], tmp[512];
    static char str[STR_MAX];
    outbuf_t o = { out, cap, 0 };
    if (cap)
        out[0] = '\0';

    const size_t n = gc_read_cstr(mem, fmt_addr, fmt, sizeof fmt);
    for (size_t i = 0; i < n;) {
        if (fmt[i] != '%') {
            put(&o, &fmt[i++], 1);
            continue;
        }
        const size_t start = i++;
        size_t sl = 0;
        spec[sl++] = '%';

        while (i < n && strchr("-+ #0", fmt[i]) && sl < SPEC_MAX - 16)
            spec[sl++] = fmt[i++];
        if (i < n && fmt[i] == '*') {               /* ancho desde los argumentos */
            sl += (size_t)snprintf(spec + sl, SPEC_MAX - sl, "%d", (int)(int32_t)gc_va_u32(va));
            i++;
        } else {
            while (i < n && fmt[i] >= '0' && fmt[i] <= '9' && sl < SPEC_MAX - 12)
                spec[sl++] = fmt[i++];
        }
        if (i < n && fmt[i] == '.') {
            spec[sl++] = fmt[i++];
            if (i < n && fmt[i] == '*') {
                const int32_t p = (int32_t)gc_va_u32(va);
                sl += (size_t)snprintf(spec + sl, SPEC_MAX - sl, "%d", p < 0 ? 0 : (int)p);
                i++;
            } else {
                while (i < n && fmt[i] >= '0' && fmt[i] <= '9' && sl < SPEC_MAX - 8)
                    spec[sl++] = fmt[i++];
            }
        }

        int len64 = 0, len_h = 0;                   /* ll, q, j: 64 bits; h y hh: se recortan */
        while (i < n && strchr("hlLqjzt", fmt[i])) {
            if (fmt[i] == 'q' || fmt[i] == 'j' || (fmt[i] == 'l' && i + 1 < n && fmt[i + 1] == 'l'))
                len64 = 1;
            if (fmt[i] == 'h')
                len_h++;
            if (fmt[i] == 'l' && i + 1 < n && fmt[i + 1] == 'l')
                i++;
            i++;
        }
        if (i >= n) {                               /* formato cortado: va tal cual */
            put(&o, &fmt[start], n - start);
            break;
        }
        const char conv = fmt[i++];
        int w = 0;
        switch (conv) {
        case 'd':
        case 'i':
            spec[sl++] = 'l';
            spec[sl++] = 'l';
            spec[sl++] = 'd';
            spec[sl] = '\0';
            {
                long long v = len64 ? (long long)(int64_t)gc_va_u64(va) : (long long)(int32_t)gc_va_u32(va);
                if (len_h == 1)
                    v = (int16_t)v;
                else if (len_h >= 2)
                    v = (int8_t)v;
                w = snprintf(tmp, sizeof tmp, spec, v);
            }
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            spec[sl++] = 'l';
            spec[sl++] = 'l';
            spec[sl++] = conv;
            spec[sl] = '\0';
            {
                unsigned long long v = len64 ? (unsigned long long)gc_va_u64(va) : gc_va_u32(va);
                if (len_h == 1)
                    v = (uint16_t)v;
                else if (len_h >= 2)
                    v = (uint8_t)v;
                w = snprintf(tmp, sizeof tmp, spec, v);
            }
            break;
        case 'c':
            spec[sl++] = 'c';
            spec[sl] = '\0';
            w = snprintf(tmp, sizeof tmp, spec, (int)(unsigned char)gc_va_u32(va));
            break;
        case 's': {
            const uint32_t p = gc_va_u32(va);
            if (p)
                gc_read_cstr(mem, p, str, sizeof str);
            else
                strcpy(str, "(null)");
            spec[sl++] = 's';
            spec[sl] = '\0';
            w = snprintf(tmp, sizeof tmp, spec, str);
            break;
        }
        case 'p':
            w = snprintf(tmp, sizeof tmp, "0x%x", (unsigned)gc_va_u32(va));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            spec[sl++] = conv;
            spec[sl] = '\0';
            w = snprintf(tmp, sizeof tmp, spec, gc_va_double(va));
            break;
        case 'n':
            gc_va_u32(va);                          /* el puntero se consume, no se escribe */
            break;
        case '%':
            tmp[0] = '%';
            w = 1;
            break;
        default:                                    /* conversión desconocida: tal cual */
            put(&o, &fmt[start], i - start);
            break;
        }
        if (w > 0)
            put(&o, tmp, (size_t)w < sizeof tmp ? (size_t)w : sizeof tmp - 1);
    }
    return o.len;
}
