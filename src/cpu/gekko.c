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
#include <string.h>

#include "core/log.h"
#include "cpu/fpu_util.h"
#include "cpu/interp.h"

gekko_op_fn gekko_op_primary[64];
gekko_op_fn gekko_op_4[1024];
gekko_op_fn gekko_op_19[1024];
gekko_op_fn gekko_op_31[1024];
gekko_op_fn gekko_op_59[32];
gekko_op_fn gekko_op_63[1024];
gekko_op_fn gekko_op_63a[32];

#define ILLEGAL_LOG_MAX 16

void gekko_op_illegal(gekko_t *c, uint32_t inst)
{
    if (c->illegal++ < ILLEGAL_LOG_MAX)
        GC_LOGW("gekko: instruccion no implementada 0x%08x en 0x%08x (op %u, xo %u)",
                (unsigned)inst, (unsigned)c->pc, (unsigned)OPCD(inst), (unsigned)XO10(inst));
    gekko_exception(c, EXC_PROGRAM, c->pc, SRR1_PROGRAM_ILLEGAL);
}

#define XO_DCBZ_L 1014

/* Opcode 4: paired singles (y dcbz_l). Sin HID2[PSE] son ilegales; sin
 * MSR[FP], FPU no disponible (en ese orden, como Dolphin). */
static void op_4(gekko_t *c, uint32_t inst)
{
    const uint32_t xo = XO10(inst);
    if (xo != XO_DCBZ_L) {
        if (!(c->spr[SPR_HID2] & HID2_PSE)) {
            gekko_exception(c, EXC_PROGRAM, c->pc, SRR1_PROGRAM_ILLEGAL);
            return;
        }
        if (gekko_fp_unavailable(c))
            return;
    }
    gekko_op_4[xo](c, inst);
}

/* Opcode 1: gancho HLE. Sin gancho registrado, es una instrucción ilegal. */
static void op_hle(gekko_t *c, uint32_t inst)
{
    if (!c->hle) {
        gekko_op_illegal(c, inst);
        return;
    }
    const uint32_t orig = c->hle(c->hle_ctx, c, inst & 0x03FFFFFFu);
    if (OPCD(orig) == GEKKO_HLE_OPCODE) {
        gekko_op_illegal(c, orig);
        return;
    }
    gekko_op_primary[OPCD(orig)](c, orig);
}

static void op_19(gekko_t *c, uint32_t inst) { gekko_op_19[XO10(inst)](c, inst); }
static void op_31(gekko_t *c, uint32_t inst) { gekko_op_31[XO10(inst)](c, inst); }

static void op_59(gekko_t *c, uint32_t inst)
{
    if (!gekko_fp_unavailable(c))
        gekko_op_59[XO5(inst)](c, inst);
}

/* En el opcode 63, las formas A (fadd, fmadd...) tienen XO5 >= 16; las X, menor. */
static void op_63(gekko_t *c, uint32_t inst)
{
    if (gekko_fp_unavailable(c))
        return;
    if (XO5(inst) & 0x10)
        gekko_op_63a[XO5(inst)](c, inst);
    else
        gekko_op_63[XO10(inst)](c, inst);
}

static void tables_init(void)
{
    static int done;
    if (done)
        return;
    for (int i = 0; i < 64; i++)
        gekko_op_primary[i] = gekko_op_illegal;
    for (int i = 0; i < 1024; i++)
        gekko_op_4[i] = gekko_op_19[i] = gekko_op_31[i] = gekko_op_63[i] = gekko_op_illegal;
    for (int i = 0; i < 32; i++)
        gekko_op_59[i] = gekko_op_63a[i] = gekko_op_illegal;
    gekko_op_primary[GEKKO_HLE_OPCODE] = op_hle;
    gekko_op_primary[4] = op_4;
    gekko_op_primary[19] = op_19;
    gekko_op_primary[31] = op_31;
    gekko_op_primary[59] = op_59;
    gekko_op_primary[63] = op_63;
    gekko_register_int();
    gekko_register_branch();
    gekko_register_mem();
    gekko_register_sys();
    gekko_register_fpu();
    gekko_register_ps();
    done = 1;
}

void gekko_reset(gekko_t *c, gc_mem_t *mem)
{
    tables_init();
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->msr = MSR_IP;
    c->pc = 0xFFF00100u;
    c->spr[SPR_PVR] = GEKKO_PVR;
    c->spr[SPR_HID1] = 0x80000000u;
    c->spr[SPR_ECID_U] = 0x0D96E200u;       /* identificador del chip: el de Dolphin */
    c->spr[SPR_ECID_M] = 0x1840C00Du;
    c->spr[SPR_ECID_L] = 0x82BB08E8u;
    c->dec = 0xFFFFFFFFu;                   /* sin bajar hasta que se escriba (Dolphin) */
    gekko_fpu_sync(c);
}

/* ---- excepciones ---------------------------------------------------------- */

void gekko_exception(gekko_t *c, uint32_t vector, uint32_t srr0, uint32_t srr1_bits)
{
    c->spr[SPR_SRR0] = srr0;
    c->spr[SPR_SRR1] = (c->msr & 0x87C0FFFFu) | srr1_bits;
    const uint32_t le = (c->msr & MSR_ILE) ? MSR_LE : 0;
    c->msr = (c->msr & ~0x04EF37u) | le;    /* apaga EE, PR, FP, IR, DR, RI, LE... */
    c->npc = ((c->msr & MSR_IP) ? 0xFFF00000u : 0) | vector;
    c->reserve = 0;
}

void gekko_set_external(gekko_t *c, int level)
{
    c->external = level;
}

/* ---- timebase y decrementador --------------------------------------------- */

/* El modelo de Dolphin (SystemTimers::DecrementerSet y DecrementerCallback):
 * con el bit de arriba en 0, DEC baja desde el valor escrito y llega a -1
 * dec × 12 ciclos después; ahí queda en 0xFFFFFFFF (no sigue bajando) y pide
 * la excepción. Escribirlo con el bit de arriba pasando de 0 a 1 la pide en
 * el acto. */
static void dec_write(gekko_t *c, uint32_t v)
{
    if (!(c->dec & 0x80000000u) && (v & 0x80000000u))
        c->dec_pending = 1;
    c->dec = v;
    c->dec_armed = 0;
    if (!(v & 0x80000000u)) {
        c->dec_start_value = v;
        c->dec_start_cycles = c->cycles;
        c->dec_when = c->cycles + (uint64_t)v * GEKKO_TB_DIVIDER;
        c->dec_armed = 1;
    }
}

static inline void advance(gekko_t *c, uint32_t cycles)
{
    c->cycles += cycles;
    if (c->dec_armed && c->cycles >= c->dec_when) {
        c->dec_armed = 0;
        c->dec = 0xFFFFFFFFu;
        c->dec_pending = 1;
    }
}

uint64_t gekko_tb(const gekko_t *c)
{
    return c->tb_start_value + (c->cycles - c->tb_start_cycles) / GEKKO_TB_DIVIDER;
}

void gekko_set_tb(gekko_t *c, uint64_t value)
{
    c->tb = value;
    c->tb_start_value = value;
    c->tb_start_cycles = c->cycles;
}

/* ---- SPR -------------------------------------------------------------------- */

/* DMA entre la caché bloqueada y la RAM (DMAU/DMAL), en bloques de 32 bytes. */
static void lc_dma(gekko_t *c, uint32_t dmal)
{
    const uint32_t dmau = c->spr[SPR_DMAU];
    const uint32_t mem_addr = dmau & ~31u;
    const uint32_t lc_off = (dmal & ~31u) & (GC_LCACHE_SIZE - 1);
    uint32_t len = (((dmau & 31u) << 2) | ((dmal >> 2) & 3u)) * 32u;
    if (len == 0)
        len = 128 * 32;
    if (len > GC_LCACHE_SIZE - lc_off)
        len = GC_LCACHE_SIZE - lc_off;
    uint8_t *ram = gc_ram_ptr(c->mem, mem_addr, len);
    if (!ram) {
        GC_LOGW("gekko: DMA de la cache bloqueada con direccion 0x%08x invalida", (unsigned)mem_addr);
        return;
    }
    if (dmal & 0x10u)                       /* LD: memoria -> caché */
        memcpy(c->mem->lcache + lc_off, ram, len);
    else
        memcpy(ram, c->mem->lcache + lc_off, len);
}

uint32_t gekko_get_spr(gekko_t *c, uint32_t spr)
{
    switch (spr) {
    case SPR_XER:   return c->xer;
    case SPR_LR:    return c->lr;
    case SPR_CTR:   return c->ctr;
    case SPR_DEC:
        if (!(c->dec & 0x80000000u))         /* todavía bajando */
            c->dec = c->dec_start_value -
                     (uint32_t)((c->cycles - c->dec_start_cycles) / GEKKO_TB_DIVIDER);
        return c->dec;
    case SPR_TBL_R: c->tb = gekko_tb(c); return (uint32_t)c->tb;
    case SPR_TBU_R: c->tb = gekko_tb(c); return (uint32_t)(c->tb >> 32);
    case SPR_PVR:   return GEKKO_PVR;
    default:        return c->spr[spr & 1023];
    }
}

void gekko_set_spr(gekko_t *c, uint32_t spr, uint32_t v)
{
    switch (spr) {
    case SPR_XER:   c->xer = v & (XER_SO | XER_OV | XER_CA | XER_BC); return;
    case SPR_LR:    c->lr = v; return;
    case SPR_CTR:   c->ctr = v; return;
    case SPR_DEC:   dec_write(c, v); return;
    /* Como Dolphin: la otra mitad es la del último valor guardado */
    case SPR_TBL_W: gekko_set_tb(c, (c->tb & 0xFFFFFFFF00000000ull) | v); return;
    case SPR_TBU_W: gekko_set_tb(c, (c->tb & 0xFFFFFFFFull) | ((uint64_t)v << 32)); return;
    case SPR_PVR:   return;                 /* solo lectura */
    case SPR_DMAL:
        c->spr[SPR_DMAL] = v & ~2u;          /* T vuelve a 0: la copia es instantánea */
        if (v & 2u)
            lc_dma(c, v);
        return;
    default:
        c->spr[spr & 1023] = v;
        return;
    }
}

/* ---- ejecución -------------------------------------------------------------- */

void gekko_step(gekko_t *c)
{
    if (c->msr & MSR_EE) {
        if (c->external) {
            gekko_exception(c, EXC_EXTERNAL, c->pc, 0);
            c->pc = c->npc;
        } else if (c->dec_pending) {
            c->dec_pending = 0;
            gekko_exception(c, EXC_DECREMENTER, c->pc, 0);
            c->pc = c->npc;
        }
    }
    const uint32_t inst = gc_read32(c->mem, c->pc & ~3u);
    if (c->trace)
        c->trace(c->trace_ctx, c->pc, inst);
    c->npc = c->pc + 4;
    gekko_op_primary[OPCD(inst)](c, inst);
    c->pc = c->npc;
    advance(c, 1);
}

uint64_t gekko_run(gekko_t *c, uint64_t cycles)
{
    const uint64_t start = c->cycles;
    c->run_end = start + cycles;
    gekko_fpu_sync(c);                      /* el redondeo del Gekko, mientras corre */
    while (c->cycles < c->run_end && !c->stop)
        gekko_step(c);
    fpu_host_round(0);                      /* el resto del emulador, al más cercano */
    return c->cycles - start;
}
