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
    GX_QUADS = 0x80, GX_QUADS_2 = 0x88, GX_TRIANGLES = 0x90, GX_TRIANGLE_STRIP = 0x98,
    GX_TRIANGLE_FAN = 0xA0, GX_LINES = 0xA8, GX_LINE_STRIP = 0xB0, GX_POINTS = 0xB8,
};
enum { BP_DRAWDONE = 0x45, BP_TOKEN = 0x47, BP_TOKEN_INT = 0x48, BP_MASK = 0xFE };
enum { BP_GENMODE = 0x00, BP_SU_SSIZE = 0x30, BP_SU_TSIZE = 0x31 };

/* Registros de CP (comando 0x08) */
enum {
    CPR_MATINDEX_A = 0x30, CPR_MATINDEX_B = 0x40, CPR_VCD_LO = 0x50, CPR_VCD_HI = 0x60,
    CPR_VAT_A = 0x70, CPR_VAT_B = 0x80, CPR_VAT_C = 0x90, CPR_ARRAY_BASE = 0xA0,
    CPR_ARRAY_STRIDE = 0xB0,
};

/* Memoria y registros de XF (XFMemory.h de Dolphin) */
enum {
    XF_POS_MTX = 0x000, XF_NORMAL_MTX = 0x400, XF_POST_MTX = 0x500, XF_LIGHTS = 0x600,
    XF_INVTXSPEC = 0x1008, XF_NUMCHAN = 0x1009, XF_AMB0 = 0x100A, XF_MAT0 = 0x100C,
    XF_COLOR0 = 0x100E, XF_ALPHA0 = 0x1010, XF_DUALTEX = 0x1012, XF_MATINDEX_A = 0x1018,
    XF_MATINDEX_B = 0x1019, XF_VIEWPORT = 0x101A, XF_PROJECTION = 0x1020, XF_NUMTEXGEN = 0x103F,
    XF_TEXMTXINFO = 0x1040, XF_POSTMTXINFO = 0x1050,
};

/* Formato de vértice: VCD (0 no está, 1 directo, 2 índice de 8 bits, 3 de 16) */
enum { VTX_NONE = 0, VTX_DIRECT = 1, VTX_INDEX8 = 2, VTX_INDEX16 = 3 };
#define VCD_POSMTX        1u
#define VCD_TEXMTX(i)     (1u << (1 + (i)))
#define VCD_POS(t)        ((uint32_t)(t) << 9)
#define VCD_NRM(t)        ((uint32_t)(t) << 11)
#define VCD_COL0(t)       ((uint32_t)(t) << 13)
#define VCD_COL1(t)       ((uint32_t)(t) << 15)
#define VCD_TEX(i, t)     ((uint32_t)(t) << (2 * (i)))          /* VCD_HI */

/* VAT grupo A: posición, normal, colores, coordenada 0 */
enum { FMT_U8 = 0, FMT_S8 = 1, FMT_U16 = 2, FMT_S16 = 3, FMT_F32 = 4 };
enum { COL_565 = 0, COL_888 = 1, COL_888X = 2, COL_4444 = 3, COL_6666 = 4, COL_8888 = 5 };
#define VAT_POS(xyz, fmt, frac) ((uint32_t)(xyz) | (uint32_t)(fmt) << 1 | (uint32_t)(frac) << 4)
#define VAT_NRM(ntb, fmt)       ((uint32_t)(ntb) << 9 | (uint32_t)(fmt) << 10)
#define VAT_COL0(fmt)           ((uint32_t)(fmt) << 14 | 1u << 13)
#define VAT_COL1(fmt)           ((uint32_t)(fmt) << 18 | 1u << 17)
#define VAT_TEX0(st, fmt, frac) ((uint32_t)(st) << 21 | (uint32_t)(fmt) << 22 | (uint32_t)(frac) << 25)
#define VAT_DEQUANT             (1u << 30)
#define VAT_INDEX3              (1u << 31)

/* Canal de color (XF 0x100E-0x1011) */
#define CH_MATSRC_VTX   1u
#define CH_LIGHT        2u
#define CH_MASK(m)      (((uint32_t)(m) & 15u) << 2 | ((uint32_t)(m) >> 4 & 15u) << 11)
#define CH_AMBSRC_VTX   (1u << 6)
#define CH_DIFF(d)      ((uint32_t)(d) << 7)        /* 0 nada, 1 con signo, 2 recortada */
#define CH_ATTN(a)      ((uint32_t)(a) << 9)        /* 0 nada, 1 especular, 2 direccional, 3 spot */

/* Coordenada de textura (XF 0x1040-0x1047) */
#define TG_STQ          2u
#define TG_ABC1         4u
#define TG_TYPE(t)      ((uint32_t)(t) << 4)        /* 0 regular, 1 emboss, 2 color 0, 3 color 1 */
#define TG_ROW(r)       ((uint32_t)(r) << 7)        /* 0 posición, 1 normal, 3-4 binormales, 5-12 coordenadas */
#define TG_EMBOSS(src, light) ((uint32_t)(src) << 12 | (uint32_t)(light) << 15)

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

/* Datos de vértices al gather pipe */
void gx_u8(uint32_t v);
void gx_u16(uint32_t v);
void gx_u32(uint32_t v);
void gx_f32(float f);
uint32_t f32_bits(float f);

/* Estado de vértices y del XF */
void gx_vat(uint32_t vat, uint32_t g0, uint32_t g1, uint32_t g2);
void gx_array(uint32_t array, const void *base, uint32_t stride);
/* VCD en CP y la cantidad de colores, normales y coordenadas en XF (INVTXSPEC),
 * que Dolphin compara: ntb dice si la normal del VAT es NTB */
void gx_vtxdesc(uint32_t lo, uint32_t hi, int ntb);
/* Índices de matriz: CP y después XF, como libogc */
void gx_matindex(uint32_t a, uint32_t b);
void gx_xf_floats(uint32_t addr, uint32_t count, const float *values);
/* Matriz de 3 × 4 en el índice `idx` (dirección idx × 4), normal de 3 × 3 en
 * 0x400 + idx × 3, post de 3 × 4 en 0x500 + idx × 4 */
void gx_pos_matrix(uint32_t idx, const float m[12]);
void gx_normal_matrix(uint32_t idx, const float m[9]);
void gx_post_matrix(uint32_t idx, const float m[12]);
/* Luz: color rrggbbaa, atenuación por ángulo y por distancia, posición y dirección */
void gx_light(uint32_t i, uint32_t color, const float cosatt[3], const float distatt[3],
              const float pos[3], const float dir[3]);
/* Proyección: 6 parámetros; ortho 0 perspectiva, 1 ortográfica */
void gx_projection(const float p[6], uint32_t ortho);

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
void dl_u16(gx_dl_t *dl, uint32_t v);
/* NOPs hasta un múltiplo de 32 y la caché de datos a la RAM; devuelve el largo. */
uint32_t dl_end(gx_dl_t *dl);

/* dcbf de cada línea y sync: la GPU lee la RAM, no la caché */
void dc_flush(const void *p, uint32_t n);

#endif
