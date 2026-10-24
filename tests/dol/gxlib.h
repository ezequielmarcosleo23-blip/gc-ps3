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
 * Mini-librería de GX para los DOLs de prueba (D-041, sin libogc): el FIFO
 * de GX (CP y PI), los comandos que se escriben en el gather pipe
 * (0xCC008000), display lists armadas en la RAM y el Pixel Engine. Cada
 * registro que se toca está a la vista; los nombres siguen a Dolphin y a
 * YAGCD.
 */
#ifndef GCPS3_TESTDOL_GXLIB_H
#define GCPS3_TESTDOL_GXLIB_H

#include <stdint.h>

#define CP_REG(o)  (*(volatile uint16_t *)(0xCC000000u + (o)))
#define PE_REG(o)  (*(volatile uint16_t *)(0xCC001000u + (o)))
#define PI_REG(o)  (*(volatile uint32_t *)(0xCC003000u + (o)))
#define WGPIPE8    (*(volatile uint8_t *)0xCC008000u)
#define WGPIPE16   (*(volatile uint16_t *)0xCC008000u)
#define WGPIPE32   (*(volatile uint32_t *)0xCC008000u)

/* Command Processor: registros de 16 bits; los pares, la mitad baja primero */
enum {
    CP_SR = 0x00, CP_CR = 0x02, CP_CLEAR = 0x04,
    CP_BASE = 0x20, CP_END = 0x24, CP_HI = 0x28, CP_LO = 0x2C,
    CP_DIST = 0x30, CP_WPTR = 0x34, CP_RPTR = 0x38, CP_BP = 0x3C,
};
enum {
    CP_SR_OVERFLOW = 0x01, CP_SR_UNDERFLOW = 0x02, CP_SR_READ_IDLE = 0x04,
    CP_SR_CMD_IDLE = 0x08, CP_SR_BREAKPOINT = 0x10,
};
enum {
    CP_CR_READ = 0x01, CP_CR_BP_ENABLE = 0x02, CP_CR_OVF_INT = 0x04, CP_CR_UNDF_INT = 0x08,
    CP_CR_LINK = 0x10, CP_CR_BP_INT = 0x20,
};

/* Pixel Engine */
enum { PE_CTRL = 0x0A, PE_TOKEN = 0x0E };
enum { PE_TOKEN_ENABLE = 0x01, PE_FINISH_ENABLE = 0x02, PE_TOKEN_ACK = 0x04, PE_FINISH_ACK = 0x08 };

/* Processor Interface */
enum {
    PI_INTSR = 0x00, PI_INTMR = 0x04, PI_FIFO_BASE = 0x0C, PI_FIFO_END = 0x10,
    PI_FIFO_WPTR = 0x14, PI_FIFO_RESET = 0x18,
};
enum { PI_INT_PE_TOKEN = 0x200, PI_INT_PE_FINISH = 0x400, PI_INT_CP = 0x800 };

/* Comandos */
enum {
    GX_NOP = 0x00, GX_LOAD_CP = 0x08, GX_LOAD_XF = 0x10, GX_LOAD_INDX_A = 0x20,
    GX_LOAD_INDX_B = 0x28, GX_LOAD_INDX_C = 0x30, GX_LOAD_INDX_D = 0x38, GX_CALL_DL = 0x40,
    GX_METRICS = 0x44, GX_INVL_VC = 0x48, GX_LOAD_BP = 0x61,
    GX_TRIANGLES = 0x90, GX_TRIANGLE_STRIP = 0x98, GX_TRIANGLE_FAN = 0xA0, GX_POINTS = 0xB8,
};
enum { BP_DRAWDONE = 0x45, BP_TOKEN = 0x47, BP_TOKEN_INT = 0x48, BP_MASK = 0xFE };

/* Dirección física (el CP y el PI la toman así) */
static inline uint32_t gx_phys(const void *p)
{
    return (uint32_t)p & 0x3FFFFFFFu;
}

static inline uint32_t mftb(void)
{
    uint32_t t;
    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}

/* Un par de registros del CP: escritura baja y alta; lectura alta primero
 * (la alta de la distancia y del puntero de lectura sincroniza la GPU). */
void     cp_write32(uint32_t lo, uint32_t v);
uint32_t cp_read32(uint32_t lo);

/* FIFO de `size` bytes (alineado a 32) en `base`, enlazado al gather pipe,
 * con sus marcas y el control indicado. */
void gx_init_fifo(void *base, uint32_t size, uint32_t hi, uint32_t lo, uint16_t ctrl);

/* Comandos al gather pipe */
void gx_bp(uint32_t reg, uint32_t value);
void gx_cp(uint32_t reg, uint32_t value);
void gx_xf(uint32_t addr, uint32_t count, const uint32_t *values);
void gx_xf1(uint32_t addr, uint32_t value);
void gx_xf_indexed(uint32_t op, uint32_t index, uint32_t addr, uint32_t size);
void gx_call_dl(const void *dl, uint32_t size);
void gx_begin(uint32_t prim, uint32_t vat, uint32_t count);

/* 32 NOPs: empuja lo que quedaba en el gather pipe (GXFlush) */
void gx_flush(void);

/* Lee SR hasta que la GPU vació el FIFO; devuelve cuántas lecturas hicieron
 * falta, o 0 si no terminó en `max`. */
uint32_t gx_wait_idle(uint32_t max);

/* Display list en la RAM */
typedef struct gx_dl {
    uint8_t *p;
    uint32_t n, cap;
} gx_dl_t;

void dl_u8(gx_dl_t *dl, uint32_t v);
void dl_u32(gx_dl_t *dl, uint32_t v);
void dl_bp(gx_dl_t *dl, uint32_t reg, uint32_t value);
void dl_cp(gx_dl_t *dl, uint32_t reg, uint32_t value);
void dl_xf1(gx_dl_t *dl, uint32_t addr, uint32_t value);
/* NOPs hasta un múltiplo de 32 y la caché de datos a la RAM; devuelve el largo. */
uint32_t dl_end(gx_dl_t *dl);

/* dcbf de cada línea y sync: la GPU lee la RAM, no la caché */
void dc_flush(const void *p, uint32_t n);

#endif
