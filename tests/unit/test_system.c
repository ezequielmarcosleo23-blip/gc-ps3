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

/* Arranque HLE de un DOL, tramos de CPU entre eventos, stop y traza. */
#include "core/log.h"
#include "core/system.h"
#include "dol_build.h"
#include "test.h"

#define TEXT0 0x80003100u

static gc_system_t sys;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static int setup(void)
{
    gc_log_set_sink(quiet);
    return gc_system_init(&sys);
}

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

/* li r3,0; bucle: addi r3,r3,1; b bucle */
static const uint32_t counter[] = { 0x38600000u, 0x38630001u, 0x4BFFFFFCu };

static void boot(const uint32_t *code, uint32_t n)
{
    static dolb_t b;
    const gc_boot_config_t cfg = { 0, 100 };          /* 100 s después del 2000-01-01 */
    dolb_init(&b);
    dolb_code(&b, 0, TEXT0, code, n);
    dolb_entry(&b, TEXT0);
    gc_system_boot_dol(&sys, b.buf, b.len, &cfg);
}

static void test_boot_state(void)
{
    static dolb_t b;
    const gc_boot_config_t cfg = { 1, 1000 };
    CHECK(setup() == 0);
    gekko_t *c = &sys.cpu;
    gc_mem_t *m = &sys.mem;

    gc_write32(m, 0x80400000u, 0xFFFFFFFFu);           /* basura de antes: se borra */
    dolb_init(&b);
    dolb_code(&b, 0, TEXT0, counter, 3);
    dolb_entry(&b, TEXT0 + 4);
    CHECK(gc_system_boot_dol(&sys, b.buf, b.len, &cfg) == GC_DOL_OK);

    CHECK(c->pc == TEXT0 + 4);
    CHECK(c->msr == 0x00002032u);
    CHECK(c->spr[SPR_HID0] == 0x0011C464u && c->spr[SPR_HID1] == 0x80000000u);
    CHECK(c->spr[SPR_HID2] == 0xE0000000u && c->spr[SPR_PVR] == GEKKO_PVR);
    CHECK(c->spr[SPR_IBAT0U] == 0x80001FFFu && c->spr[SPR_IBAT0L] == 2);
    CHECK(c->spr[SPR_DBAT1U] == 0xC0001FFFu && c->spr[SPR_DBAT1L] == 0x2A);
    CHECK(c->spr[SPR_ECID_U] == 0x0D96E200u);
    CHECK(gc_read32(m, 0x80000900u) == 0x4C000064u && gc_read32(m, 0x80001700u) == 0x4C000064u);
    CHECK(gc_read32(m, 0x80000020u) == 0x0D15EA5Eu);
    CHECK(gc_read32(m, 0x80000028u) == 0x01800000u);
    CHECK(gc_read32(m, 0x8000002Cu) == 0x10000006u);
    CHECK(gc_read32(m, 0x800000CCu) == 1);              /* PAL */
    CHECK(gc_read32(m, 0x800000D0u) == 0x01000000u);
    CHECK(gc_read32(m, 0x800000F8u) == 162000000u && gc_read32(m, 0x800000FCu) == 486000000u);
    CHECK(gc_read64(m, 0x800030D8u) == 1000ull * 40500000ull);
    CHECK(gc_read32(m, 0x80400000u) == 0);
    CHECK(gc_read32(m, TEXT0 + 4) == 0x38630001u);

    /* Un DOL roto no toca nada */
    c->pc = 0x1234;
    b.buf[0x90 + 3] = 0;                                /* sin texto */
    b.buf[0x90 + 2] = 0;
    CHECK(gc_system_boot_dol(&sys, b.buf, b.len, &cfg) == GC_DOL_ERR_EMPTY);
    CHECK(c->pc == 0x1234 && gc_read32(m, TEXT0 + 4) == 0x38630001u);
    teardown();
}

static uint64_t seen_at[4];
static int seen;

static void record(void *ctx, uint64_t when, uint64_t late)
{
    (void)when;
    (void)late;
    seen_at[seen++] = ((gc_system_t *)ctx)->cpu.cycles;
}

static void stop_cpu(void *ctx, uint64_t when, uint64_t late)
{
    (void)when;
    (void)late;
    ((gc_system_t *)ctx)->cpu.stop = 1;
}

/* Los eventos se atienden en su ciclo exacto; stop corta el tramo. */
static void test_run_slices(void)
{
    CHECK(setup() == 0);
    boot(counter, 3);
    seen = 0;
    CHECK(gc_sched_add(&sys.sched, 31, "r", record, &sys) == 0);
    CHECK(gc_sched_add(&sys.sched, 31, "r", record, &sys) == 0);
    CHECK(gc_sched_add(&sys.sched, 45, "r", record, &sys) == 0);
    CHECK(gc_sched_add(&sys.sched, 60, "s", stop_cpu, &sys) == 0);

    CHECK(gc_system_run(&sys, 1000) == 60);
    CHECK(seen == 3 && seen_at[0] == 31 && seen_at[1] == 31 && seen_at[2] == 45);
    sys.cpu.stop = 0;
    CHECK(gc_system_run(&sys, 41) == 41);
    CHECK(sys.cpu.cycles == 101);
    CHECK(sys.cpu.gpr[3] == 50 && sys.cpu.pc == TEXT0 + 4);
    teardown();
}

/* Con los rfi del IPL en los vectores, un decrementador no rompe el programa. */
static void test_rfi_vectors(void)
{
    static const uint32_t code[] = {
        0x38600000u,    /* li r3,0 */
        0x38A00005u,    /* li r5,5 */
        0x7CB603A6u,    /* mtdec r5 */
        0x7C8000A6u,    /* mfmsr r4 */
        0x60848000u,    /* ori r4,r4,0x8000 (EE) */
        0x7C800124u,    /* mtmsr r4 */
        0x38630001u,    /* bucle: addi r3,r3,1 */
        0x4BFFFFFCu,    /* b bucle */
    };
    CHECK(setup() == 0);
    boot(code, 8);
    CHECK(gc_system_run(&sys, 2000) == 2000);
    CHECK(sys.cpu.spr[SPR_SRR0] == TEXT0 + 0x18 || sys.cpu.spr[SPR_SRR0] == TEXT0 + 0x1C);
    CHECK(sys.cpu.pc == TEXT0 + 0x18 || sys.cpu.pc == TEXT0 + 0x1C);
    CHECK(sys.cpu.msr & MSR_EE);                        /* rfi lo devolvió */
    CHECK(sys.cpu.gpr[3] > 900);
    teardown();
}

static uint32_t trace_pc[8];
static int ntrace;

static void trace(void *ctx, uint32_t pc, uint32_t inst)
{
    (void)ctx;
    (void)inst;
    if (ntrace < 8)
        trace_pc[ntrace] = pc;
    ntrace++;
}

static void test_trace(void)
{
    CHECK(setup() == 0);
    boot(counter, 3);
    ntrace = 0;
    sys.cpu.trace = trace;
    CHECK(gc_system_run(&sys, 5) == 5);
    CHECK(ntrace == 5);
    CHECK(trace_pc[0] == TEXT0 && trace_pc[1] == TEXT0 + 4 && trace_pc[2] == TEXT0 + 8);
    CHECK(trace_pc[3] == TEXT0 + 4 && trace_pc[4] == TEXT0 + 8);
    sys.cpu.trace = NULL;
    teardown();
}

const test_case_t system_tests[] = {
    { "sistema: estado del arranque HLE (MSR, HID, BAT, vectores, memoria baja)", test_boot_state },
    { "sistema: eventos en su ciclo exacto y stop", test_run_slices },
    { "sistema: decrementador con los rfi del IPL", test_rfi_vectors },
    { "sistema: traza de instrucciones", test_trace },
    { NULL, NULL },
};
