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

/* Mini-librería de GX para los DOLs de prueba (ver gxlib.h). */
#include "gxlib.h"

void cp_write32(uint32_t lo, uint32_t v)
{
    CP_REG(lo) = (uint16_t)v;
    CP_REG(lo + 2) = (uint16_t)(v >> 16);
}

uint32_t cp_read32(uint32_t lo)
{
    const uint32_t hi = CP_REG(lo + 2);
    return hi << 16 | CP_REG(lo);
}

void gx_init_fifo(void *base, uint32_t size, uint32_t hi, uint32_t lo, uint16_t ctrl)
{
    const uint32_t b = gx_phys(base), e = b + size - 32;
    CP_REG(CP_CR) = 0;                           /* sin leer ni enlazar mientras se arma */
    CP_REG(CP_CLEAR) = 3;
    cp_write32(CP_BASE, b);
    cp_write32(CP_END, e);
    cp_write32(CP_HI, hi);
    cp_write32(CP_LO, lo);
    cp_write32(CP_DIST, 0);
    cp_write32(CP_WPTR, b);
    cp_write32(CP_RPTR, b);
    PI_REG(PI_FIFO_BASE) = b;
    PI_REG(PI_FIFO_END) = e;
    PI_REG(PI_FIFO_WPTR) = b;
    __asm__ volatile("sync" : : : "memory");
    CP_REG(CP_CR) = ctrl;
}

void gx_bp(uint32_t reg, uint32_t value)
{
    WGPIPE8 = GX_LOAD_BP;
    WGPIPE32 = reg << 24 | (value & 0xFFFFFFu);
}

void gx_cp(uint32_t reg, uint32_t value)
{
    WGPIPE8 = GX_LOAD_CP;
    WGPIPE8 = (uint8_t)reg;
    WGPIPE32 = value;
}

void gx_xf(uint32_t addr, uint32_t count, const uint32_t *values)
{
    WGPIPE8 = GX_LOAD_XF;
    WGPIPE32 = (count - 1) << 16 | addr;
    for (uint32_t k = 0; k < count; k++)
        WGPIPE32 = values[k];
}

void gx_xf1(uint32_t addr, uint32_t value)
{
    gx_xf(addr, 1, &value);
}

void gx_xf_indexed(uint32_t op, uint32_t index, uint32_t addr, uint32_t size)
{
    WGPIPE8 = (uint8_t)op;
    WGPIPE32 = index << 16 | (size - 1) << 12 | addr;
}

void gx_call_dl(const void *dl, uint32_t size)
{
    WGPIPE8 = GX_CALL_DL;
    WGPIPE32 = gx_phys(dl);
    WGPIPE32 = size;
}

void gx_begin(uint32_t prim, uint32_t vat, uint32_t count)
{
    WGPIPE8 = (uint8_t)(prim | vat);
    WGPIPE16 = (uint16_t)count;
}

void gx_flush(void)
{
    for (int k = 0; k < 8; k++)
        WGPIPE32 = 0;
    __asm__ volatile("sync" : : : "memory");
}

uint32_t gx_wait_idle(uint32_t max)
{
    const uint16_t idle = CP_SR_READ_IDLE | CP_SR_CMD_IDLE;
    for (uint32_t n = 1; n <= max; n++)
        if ((CP_REG(CP_SR) & idle) == idle)
            return n;
    return 0;
}

void dl_u8(gx_dl_t *dl, uint32_t v)
{
    if (dl->n < dl->cap)
        dl->p[dl->n++] = (uint8_t)v;
}

void dl_u32(gx_dl_t *dl, uint32_t v)
{
    for (int k = 24; k >= 0; k -= 8)
        dl_u8(dl, v >> k);
}

void dl_bp(gx_dl_t *dl, uint32_t reg, uint32_t value)
{
    dl_u8(dl, GX_LOAD_BP);
    dl_u32(dl, reg << 24 | (value & 0xFFFFFFu));
}

void dl_cp(gx_dl_t *dl, uint32_t reg, uint32_t value)
{
    dl_u8(dl, GX_LOAD_CP);
    dl_u8(dl, reg);
    dl_u32(dl, value);
}

void dl_xf1(gx_dl_t *dl, uint32_t addr, uint32_t value)
{
    dl_u8(dl, GX_LOAD_XF);
    dl_u32(dl, addr);
    dl_u32(dl, value);
}

uint32_t dl_end(gx_dl_t *dl)
{
    while (dl->n & 31u)
        dl_u8(dl, GX_NOP);
    dc_flush(dl->p, dl->n);
    return dl->n;
}

void dc_flush(const void *p, uint32_t n)
{
    const uint32_t start = (uint32_t)p & ~31u, end = (uint32_t)p + n;
    for (uint32_t a = start; a < end; a += 32)
        __asm__ volatile("dcbf 0, %0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}
