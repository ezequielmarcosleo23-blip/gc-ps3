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
 * Intérprete de Gekko: saltos, memoria, reservas, cadenas, SPR, excepciones,
 * timebase y decrementador, con programas cortos ensamblados a mano.
 */
#include <string.h>

#include "core/log.h"
#include "core/mem.h"
#include "cpu/gekko.h"
#include "test.h"

#define CODE 0x80003000u

/* ---- codificación ------------------------------------------------------------ */

static uint32_t D(uint32_t op, uint32_t rd, uint32_t ra, int32_t imm)
{
    return op << 26 | rd << 21 | ra << 16 | ((uint32_t)imm & 0xFFFFu);
}

static uint32_t X(uint32_t rd, uint32_t ra, uint32_t rb, uint32_t xo, uint32_t rc)
{
    return 31u << 26 | rd << 21 | ra << 16 | rb << 11 | xo << 1 | rc;
}

static uint32_t XL(uint32_t bo, uint32_t bi, uint32_t xo, uint32_t lk)
{
    return 19u << 26 | bo << 21 | bi << 16 | xo << 1 | lk;
}

static uint32_t B(int32_t off, uint32_t aa, uint32_t lk)
{
    return 18u << 26 | ((uint32_t)off & 0x03FFFFFCu) | aa << 1 | lk;
}

static uint32_t BC(uint32_t bo, uint32_t bi, int32_t off, uint32_t lk)
{
    return 16u << 26 | bo << 21 | bi << 16 | ((uint32_t)off & 0xFFFCu) | lk;
}

static uint32_t SPR(uint32_t xo, uint32_t r, uint32_t spr)
{
    return X(r, spr & 31, spr >> 5, xo, 0);
}

#define ADDI(rd, ra, i) D(14, rd, ra, i)
#define LWZ(rd, ra, i)  D(32, rd, ra, i)
#define STW(rs, ra, i)  D(36, rs, ra, i)
#define MTSPR(spr, rs)  SPR(467, rs, spr)
#define MFSPR(rd, spr)  SPR(339, rd, spr)
#define NOP             D(24, 0, 0, 0)

static gc_mem_t mem;
static gekko_t cpu;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static int boot(const uint32_t *code, int n)
{
    gc_log_set_sink(quiet);
    if (gc_mem_init(&mem) != 0)
        return -1;
    gekko_reset(&cpu, &mem);
    for (int k = 0; k < n; k++)
        gc_write32(&mem, CODE + 4u * (uint32_t)k, code[k]);
    cpu.pc = CODE;
    cpu.msr = MSR_FP;             /* como un juego: traducción encendida, sin EE */
    return 0;
}

static void done(void)
{
    gc_mem_free(&mem);
    gc_log_set_sink(NULL);
}

/* ---- saltos -------------------------------------------------------------------- */

static void test_bdnz_loop(void)
{
    /* r3 = 0; ctr = 5; loop: r3 += 2; bdnz loop; nop */
    const uint32_t code[] = {
        ADDI(3, 0, 0), ADDI(4, 0, 5), MTSPR(9, 4),
        ADDI(3, 3, 2), BC(16, 0, -4, 0), NOP,
    };
    CHECK(boot(code, 6) == 0);
    gekko_run(&cpu, 3 + 5 * 2 + 1);
    uint32_t r3 = cpu.gpr[3], ctr = cpu.ctr, pc = cpu.pc;
    done();
    CHECK_EQ_U64(r3, 10);
    CHECK_EQ_U64(ctr, 0);
    CHECK_EQ_U64(pc, CODE + 24);
}

static void test_call_and_return(void)
{
    /* bl +12; addi r5,0,7; b +12 (fin); func: addi r6,0,9; blr; fin: nop */
    const uint32_t code[] = {
        B(12, 0, 1), ADDI(5, 0, 7), B(12, 0, 0),
        ADDI(6, 0, 9), XL(20, 0, 16, 0), NOP,
    };
    CHECK(boot(code, 6) == 0);
    gekko_step(&cpu);                              /* bl */
    uint32_t lr = cpu.lr, at_func = cpu.pc;
    gekko_run(&cpu, 4);                            /* addi r6, blr, addi r5, b */
    uint32_t r5 = cpu.gpr[5], r6 = cpu.gpr[6], pc = cpu.pc;
    done();
    CHECK_EQ_U64(lr, CODE + 4);
    CHECK_EQ_U64(at_func, CODE + 12);
    CHECK(r5 == 7 && r6 == 9);
    CHECK_EQ_U64(pc, CODE + 20);
}

static void test_conditional_and_ctr(void)
{
    /* cmpwi r3,5 ; beq +8 ; addi r4,0,1 ; addi r4,r4,10 ; mtctr ; bctrl */
    const uint32_t code[] = {
        D(11, 0, 3, 5), BC(12, 2, 8, 0), ADDI(4, 0, 1), ADDI(4, 4, 10),
        MTSPR(9, 7), XL(20, 0, 528, 1),
    };
    CHECK(boot(code, 6) == 0);
    cpu.gpr[3] = 5;
    cpu.gpr[7] = 0x80005000u;
    gekko_run(&cpu, 4);                            /* cmpwi, beq (salta el addi r4,0,1), addi, mtctr */
    uint32_t r4 = cpu.gpr[4];
    gekko_step(&cpu);                              /* bctrl */
    uint32_t pc = cpu.pc, lr = cpu.lr;
    done();
    CHECK_EQ_U64(r4, 10);
    CHECK_EQ_U64(pc, 0x80005000u);
    CHECK_EQ_U64(lr, CODE + 24);
}

static void test_bcl_sets_lr_when_not_taken(void)
{
    const uint32_t code[] = { BC(4, 2, 100, 1), NOP };   /* bnel con EQ = 1: no salta */
    CHECK(boot(code, 2) == 0);
    cpu.cr = 0x20000000u;
    gekko_step(&cpu);
    uint32_t pc = cpu.pc, lr = cpu.lr;
    done();
    CHECK_EQ_U64(pc, CODE + 4);
    CHECK_EQ_U64(lr, CODE + 4);
}

static void test_cr_ops(void)
{
    /* cror 2,0,1 ; crxor 3,3,3 ; mcrf 7,0 ; mtcrf 0x01,r5 ; mcrxr 1 */
    const uint32_t code[] = {
        XL(2, 0, 449, 0) | (1u << 11), XL(3, 3, 193, 0) | (3u << 11),
        19u << 26 | 7u << 23 | 0u << 18,
        X(5, 0, 0, 144, 0) | (0x01u << 12),
        X(1u << 2, 0, 0, 512, 0),
    };
    CHECK(boot(code, 5) == 0);
    cpu.cr = 0x80000000u | 0x10000000u;            /* LT y SO en CR0 */
    cpu.gpr[5] = 0x0000000Au;
    cpu.xer = 0xE0000000u;
    gekko_run(&cpu, 5);
    uint32_t cr = cpu.cr, xer = cpu.xer;
    done();
    /* CR0 = 1010 (LT, EQ = LT|GT, SO por crxor = 0); CR7 = copia de CR0 tras mcrf
       y luego mtcrf pone CR7 = 0xA; CR1 = XER[0..3] = 0xE */
    CHECK_EQ_U64(cr >> 28, 0xA);
    CHECK_EQ_U64((cr >> 24) & 0xF, 0xE);
    CHECK_EQ_U64(cr & 0xF, 0xA);
    CHECK_EQ_U64(xer, 0);
}

/* ---- memoria ------------------------------------------------------------------- */

static void test_loads_stores(void)
{
    const uint32_t code[] = {
        STW(3, 10, 0),                     /* stw r3,0(r10) */
        D(37, 3, 10, 8),                   /* stwu r3,8(r10): r10 += 8 */
        D(42, 4, 11, 0),                   /* lha r4,0(r11): 0x8000 con signo */
        D(40, 5, 11, 0),                   /* lhz r5,0(r11) */
        D(34, 6, 11, 1),                   /* lbz r6,1(r11) */
        X(7, 0, 12, 534, 0),               /* lwbrx r7,0,r12 */
        X(3, 0, 13, 918, 0),               /* sthbrx r3,0,r13 */
    };
    CHECK(boot(code, 7) == 0);
    cpu.gpr[3] = 0x11223344u;
    cpu.gpr[10] = 0x80010000u;
    cpu.gpr[11] = 0x80020000u;
    cpu.gpr[12] = 0x80010000u;
    cpu.gpr[13] = 0x80030000u;
    gc_write16(&mem, 0x80020000u, 0x8001u);
    gekko_run(&cpu, 7);
    uint32_t w0 = gc_read32(&mem, 0x80010000u), w8 = gc_read32(&mem, 0x80010008u);
    uint32_t r10 = cpu.gpr[10], r4 = cpu.gpr[4], r5 = cpu.gpr[5], r6 = cpu.gpr[6], r7 = cpu.gpr[7];
    uint16_t h = gc_read16(&mem, 0x80030000u);
    done();
    CHECK_EQ_U64(w0, 0x11223344u);
    CHECK_EQ_U64(w8, 0x11223344u);
    CHECK_EQ_U64(r10, 0x80010008u);
    CHECK_EQ_U64(r4, 0xFFFF8001u);
    CHECK_EQ_U64(r5, 0x8001u);
    CHECK_EQ_U64(r6, 0x01u);
    CHECK_EQ_U64(r7, 0x44332211u);
    CHECK_EQ_U64(h, 0x4433u);
}

static void test_lmw_stmw(void)
{
    const uint32_t code[] = { D(47, 28, 10, 0), D(46, 24, 10, 0) };  /* stmw r28 ; lmw r24 */
    CHECK(boot(code, 2) == 0);
    for (uint32_t r = 28; r < 32; r++)
        cpu.gpr[r] = 0xA0000000u + r;
    cpu.gpr[10] = 0x80040000u;
    gekko_run(&cpu, 2);
    uint32_t m = gc_read32(&mem, 0x8004000Cu), r24 = cpu.gpr[24], r27 = cpu.gpr[27];
    done();
    CHECK_EQ_U64(m, 0xA000001Fu);
    CHECK_EQ_U64(r24, 0xA000001Cu);
    CHECK_EQ_U64(r27, 0xA000001Fu);
}

static void test_strings(void)
{
    /* stswi r5,r10,7 ; lswi r20,r10,7 */
    const uint32_t code[] = { X(5, 10, 7, 725, 0), X(20, 10, 7, 597, 0) };
    CHECK(boot(code, 2) == 0);
    cpu.gpr[5] = 0x41424344u;
    cpu.gpr[6] = 0x45464748u;
    cpu.gpr[10] = 0x80050000u;
    gc_write8(&mem, 0x80050007u, 0x77);
    gekko_run(&cpu, 2);
    uint8_t b6 = gc_read8(&mem, 0x80050006u), b7 = gc_read8(&mem, 0x80050007u);
    uint32_t r20 = cpu.gpr[20], r21 = cpu.gpr[21];
    done();
    CHECK_EQ_U64(b6, 0x47);
    CHECK_EQ_U64(b7, 0x77);                         /* el 8.º byte no se toca */
    CHECK_EQ_U64(r20, 0x41424344u);
    CHECK_EQ_U64(r21, 0x45464700u);
}

static void test_reservation(void)
{
    /* lwarx r3,0,r10 ; stwcx. r4,0,r10 ; stwcx. r5,0,r10 */
    const uint32_t code[] = { X(3, 0, 10, 20, 0), X(4, 0, 10, 150, 1), X(5, 0, 10, 150, 1) };
    CHECK(boot(code, 3) == 0);
    cpu.gpr[4] = 0x1111u;
    cpu.gpr[5] = 0x2222u;
    cpu.gpr[10] = 0x80060000u;
    gekko_run(&cpu, 2);
    uint32_t cr_ok = cpu.cr >> 28;
    gekko_step(&cpu);
    uint32_t cr_fail = cpu.cr >> 28, v = gc_read32(&mem, 0x80060000u);
    done();
    CHECK_EQ_U64(cr_ok, 0x2);
    CHECK_EQ_U64(cr_fail, 0x0);
    CHECK_EQ_U64(v, 0x1111u);
}

static void test_dcbz_and_locked_cache(void)
{
    /* dcbz 0,r10 ; dcbz_l 0,r11 ; mtspr DMAU,r12 ; mtspr DMAL,r13 */
    const uint32_t code[] = {
        X(0, 0, 10, 1014, 0), (4u << 26) | (11u << 11) | (1014u << 1),
        MTSPR(SPR_DMAU, 12), MTSPR(SPR_DMAL, 13),
    };
    CHECK(boot(code, 4) == 0);
    for (uint32_t k = 0; k < 64; k += 4)
        gc_write32(&mem, 0x80070000u + k, 0xFFFFFFFFu);
    for (uint32_t k = 0; k < 64; k++)
        gc_write8(&mem, 0x80080000u + k, (uint8_t)k);
    gc_write32(&mem, 0xE0000020u, 0xFFFFFFFFu);
    cpu.gpr[10] = 0x80070010u;                       /* dcbz alinea a 32 */
    cpu.gpr[11] = 0xE0000024u;
    cpu.gpr[12] = 0x80080000u | 0;                   /* longitud alta = 0 */
    cpu.gpr[13] = 0xE0000100u | 0x10u | (2u << 2) | 2u;  /* LD, 2 líneas, T */
    gekko_run(&cpu, 4);
    uint32_t z0 = gc_read32(&mem, 0x80070000u), z1 = gc_read32(&mem, 0x8007001Cu);
    uint32_t keep = gc_read32(&mem, 0x80070020u), lc = gc_read32(&mem, 0xE0000020u);
    uint32_t dma0 = gc_read32(&mem, 0xE0000100u), dma1 = gc_read32(&mem, 0xE000013Cu);
    uint32_t dma_end = gc_read32(&mem, 0xE0000140u), dmal = cpu.spr[SPR_DMAL];
    done();
    CHECK(z0 == 0 && z1 == 0);
    CHECK_EQ_U64(keep, 0xFFFFFFFFu);
    CHECK_EQ_U64(lc, 0);
    CHECK_EQ_U64(dma0, 0x00010203u);
    CHECK_EQ_U64(dma1, 0x3C3D3E3Fu);
    CHECK_EQ_U64(dma_end, 0);
    CHECK((dmal & 2u) == 0);
}

/* ---- sistema y excepciones ------------------------------------------------------ */

static void test_spr_and_timebase(void)
{
    const uint32_t code[] = {
        MTSPR(272, 3), MFSPR(4, 272), MFSPR(5, SPR_PVR),
        X(6, 268 & 31, 268 >> 5, 371, 0),           /* mftb r6 */
    };
    CHECK(boot(code, 4) == 0);
    cpu.gpr[3] = 0xCAFEu;
    gekko_set_tb(&cpu, 100);
    gekko_run(&cpu, 4);
    uint32_t r4 = cpu.gpr[4], r5 = cpu.gpr[5], r6 = cpu.gpr[6];
    gekko_run(&cpu, 0);
    done();
    CHECK_EQ_U64(r4, 0xCAFEu);
    CHECK_EQ_U64(r5, GEKKO_PVR);
    CHECK_EQ_U64(r6, 100);                          /* 3 instrucciones < 12 ciclos */
}

static void test_syscall_and_rfi(void)
{
    const uint32_t code[] = { 17u << 26 | 2u, NOP };   /* sc */
    CHECK(boot(code, 2) == 0);
    cpu.msr = MSR_FP | MSR_EE | MSR_IR | MSR_DR;
    gc_write32(&mem, 0x80000C00u, XL(0, 0, 50, 0));   /* rfi en el vector */
    gekko_step(&cpu);
    uint32_t pc = cpu.pc, srr0 = cpu.spr[SPR_SRR0], srr1 = cpu.spr[SPR_SRR1], msr = cpu.msr;
    gekko_step(&cpu);                                  /* rfi */
    uint32_t back = cpu.pc, msr_back = cpu.msr;
    done();
    CHECK_EQ_U64(pc, 0x00000C00u);
    CHECK_EQ_U64(srr0, CODE + 4);
    CHECK_EQ_U64(srr1 & 0xFFFFu, MSR_FP | MSR_EE | MSR_IR | MSR_DR);
    CHECK((msr & (MSR_EE | MSR_IR | MSR_DR | MSR_FP)) == 0);
    CHECK_EQ_U64(back, CODE + 4);
    CHECK_EQ_U64(msr_back, MSR_FP | MSR_EE | MSR_IR | MSR_DR);
}

static void test_trap_and_illegal(void)
{
    const uint32_t code[] = { D(3, 4, 3, 0), 0x00000000u };   /* tweqi r3,0 ; instrucción 0 */
    CHECK(boot(code, 2) == 0);
    cpu.gpr[3] = 0;
    gekko_step(&cpu);
    uint32_t pc1 = cpu.pc, srr0_1 = cpu.spr[SPR_SRR0], srr1_1 = cpu.spr[SPR_SRR1];
    cpu.pc = CODE + 4;
    gekko_step(&cpu);
    uint32_t pc2 = cpu.pc, srr1_2 = cpu.spr[SPR_SRR1], illegal = cpu.illegal;
    done();
    CHECK_EQ_U64(pc1, 0x700);
    CHECK_EQ_U64(srr0_1, CODE);
    CHECK(srr1_1 & SRR1_PROGRAM_TRAP);
    CHECK_EQ_U64(pc2, 0x700);
    CHECK(srr1_2 & SRR1_PROGRAM_ILLEGAL);
    CHECK_EQ_U64(illegal, 1);
}

/* El decrementador como en Dolphin: llega a -1 dec × 12 ciclos después de
 * escribirlo, ahí queda en 0xFFFFFFFF y pide la excepción */
static void test_decrementer(void)
{
    uint32_t code[64];
    for (int k = 0; k < 64; k++)
        code[k] = NOP;
    CHECK(boot(code, 64) == 0);
    const uint64_t c0 = cpu.cycles;
    gekko_set_spr(&cpu, SPR_DEC, 2);
    gc_write32(&mem, 0x80000900u, NOP);  /* vector del decrementador */
    cpu.msr |= MSR_EE;
    gekko_run(&cpu, 23);                 /* 23 ciclos: 1 tick, DEC = 1 */
    uint32_t dec_a = gekko_get_spr(&cpu, SPR_DEC), pc_a = cpu.pc;
    int pending_a = cpu.dec_pending;
    gekko_run(&cpu, 1);                  /* el ciclo 24 lo pasa a -1 */
    int pending = cpu.dec_pending;
    gekko_step(&cpu);                    /* se toma la excepción */
    uint32_t pc_b = cpu.pc, srr0 = cpu.spr[SPR_SRR0];
    uint32_t dec_b = gekko_get_spr(&cpu, SPR_DEC);
    uint64_t tb = gekko_tb(&cpu) - c0 / 12;
    /* Escribirlo con el bit de arriba de 0 a 1 la pide en el acto */
    cpu.msr &= ~MSR_EE;
    gekko_set_spr(&cpu, SPR_DEC, 100);
    gekko_set_spr(&cpu, SPR_DEC, 0x80000000u);
    int pending_sw = cpu.dec_pending, armed = cpu.dec_armed;
    done();
    CHECK_EQ_U64(dec_a, 1);
    CHECK_EQ_U64(pc_a, CODE + 23 * 4);
    CHECK(pending_a == 0 && pending == 1);
    CHECK_EQ_U64(pc_b, 0x900 + 4);       /* ya ejecutó la primera del vector */
    CHECK_EQ_U64(srr0, CODE + 24 * 4);
    CHECK_EQ_U64(dec_b, 0xFFFFFFFFu);    /* no sigue bajando (Dolphin) */
    CHECK_EQ_U64(tb, 2);
    CHECK(pending_sw == 1 && armed == 0);
}

static void test_external_needs_ee(void)
{
    const uint32_t code[] = { NOP, NOP, NOP };
    CHECK(boot(code, 3) == 0);
    gc_write32(&mem, 0x80000500u, NOP);  /* vector de la interrupción externa */
    gekko_set_external(&cpu, 1);
    gekko_step(&cpu);                    /* sin EE: sigue */
    uint32_t pc_a = cpu.pc;
    cpu.msr |= MSR_EE;
    gekko_step(&cpu);
    uint32_t pc_b = cpu.pc, srr0 = cpu.spr[SPR_SRR0];
    done();
    CHECK_EQ_U64(pc_a, CODE + 4);
    CHECK_EQ_U64(pc_b, 0x500 + 4);
    CHECK_EQ_U64(srr0, CODE + 4);
}

const test_case_t cpu_tests[] = {
    { "gekko: bucle con bdnz",                 test_bdnz_loop },
    { "gekko: bl y blr",                       test_call_and_return },
    { "gekko: cmpwi, beq y bctrl",             test_conditional_and_ctr },
    { "gekko: bcl guarda LR aunque no salte",  test_bcl_sets_lr_when_not_taken },
    { "gekko: cror, crxor, mcrf, mtcrf, mcrxr", test_cr_ops },
    { "gekko: lecturas y escrituras",          test_loads_stores },
    { "gekko: lmw y stmw",                     test_lmw_stmw },
    { "gekko: lswi y stswi",                   test_strings },
    { "gekko: lwarx y stwcx.",                 test_reservation },
    { "gekko: dcbz, dcbz_l y DMA de la cache bloqueada", test_dcbz_and_locked_cache },
    { "gekko: SPR, PVR y timebase",            test_spr_and_timebase },
    { "gekko: sc y rfi",                       test_syscall_and_rfi },
    { "gekko: trap e instruccion ilegal",      test_trap_and_illegal },
    { "gekko: decrementador",                  test_decrementer },
    { "gekko: interrupcion externa con EE",    test_external_needs_ee },
    { NULL, NULL },
};
