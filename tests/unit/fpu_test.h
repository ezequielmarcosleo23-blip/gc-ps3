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

/* Ayudas comunes de test_cpu_fpu.c y test_cpu_ps.c: codificación, valores y
 * una CPU con la FPU y los paired singles habilitados. */
#ifndef GCPS3_TESTS_FPU_TEST_H
#define GCPS3_TESTS_FPU_TEST_H

#include <string.h>

#include "core/log.h"
#include "core/mem.h"
#include "cpu/gekko.h"
#include "test.h"

#define FT_CODE 0x80001000u
#define FT_DATA 0x80002000u

/* ---- codificación ------------------------------------------------------------ */

static inline uint32_t enc_a(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t c, uint32_t xo, uint32_t rc)
{
    return op << 26 | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1 | rc;
}

static inline uint32_t enc_x(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t xo, uint32_t rc)
{
    return op << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc;
}

static inline uint32_t enc_d(uint32_t op, uint32_t d, uint32_t a, int32_t imm)
{
    return op << 26 | d << 21 | a << 16 | ((uint32_t)imm & 0xFFFFu);
}

/* psq_l/psq_st: W, I y desplazamiento de 12 bits */
static inline uint32_t enc_psq(uint32_t op, uint32_t d, uint32_t a, uint32_t w, uint32_t i, int32_t disp)
{
    return op << 26 | d << 21 | a << 16 | w << 15 | i << 12 | ((uint32_t)disp & 0xFFFu);
}

/* psq_lx/psq_stx/psq_lux/psq_stux: opcode 4, XO de 6 bits */
static inline uint32_t enc_psqx(uint32_t d, uint32_t a, uint32_t b, uint32_t w, uint32_t i, uint32_t xo6)
{
    return 4u << 26 | d << 21 | a << 16 | b << 11 | w << 10 | i << 7 | xo6 << 1;
}

/* ---- valores ------------------------------------------------------------------- */

static inline uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double   dval(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

/* float -> bits de double, como lfs (sin pasar por la FPU: no silencia SNaN) */
static inline uint64_t sbits(uint32_t s)
{
    if ((s & 0x7F800000u) == 0x7F800000u && (s & 0x007FFFFFu))
        return (uint64_t)(s & 0x80000000u) << 32 | 0x7FF0000000000000ull | (uint64_t)(s & 0x007FFFFFu) << 29;
    float f;
    memcpy(&f, &s, 4);
    return dbits((double)f);
}

/* ---- CPU -------------------------------------------------------------------------- */

static gc_mem_t ft_mem;
static gekko_t ft_cpu;

static inline void ft_quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

/* MSR[FP], HID2[PSE|LSQE], FPSCR = 0 (redondeo al más cercano) */
static inline int ft_setup(void)
{
    gc_log_set_sink(ft_quiet);
    if (gc_mem_init(&ft_mem) != 0)
        return -1;
    gekko_reset(&ft_cpu, &ft_mem);
    ft_cpu.msr = MSR_FP;
    ft_cpu.spr[SPR_HID2] = HID2_PSE | HID2_LSQE;
    return 0;
}

static inline void ft_teardown(void)
{
    ft_cpu.fpscr = 0;
    gekko_fpu_sync(&ft_cpu);
    gc_mem_free(&ft_mem);
    gc_log_set_sink(NULL);
}

static inline void ft_rn(uint32_t rn)
{
    ft_cpu.fpscr = (ft_cpu.fpscr & ~FPSCR_RN) | (rn & 3);
    gekko_fpu_sync(&ft_cpu);
}

/* Ejecuta una instrucción en FT_CODE. */
static inline void ft_exec(uint32_t inst)
{
    gc_write32(&ft_mem, FT_CODE, inst);
    ft_cpu.pc = FT_CODE;
    gekko_step(&ft_cpu);
}

#endif
