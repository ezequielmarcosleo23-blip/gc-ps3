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
 * FPU escalar contra el oráculo (tests/unit/data/oracle_fpu.bin, generado por
 * tools/ppc-oracle/oracle_fpu.c en un 750CXe de QEMU) y casos a mano de lo que
 * el oráculo no cubre: fres/frsqrte del Gekko, FPSCR[NI], FEX y trampas,
 * MSR[FP], alineación y las formas con actualización.
 *
 * Del FPSCR no se comparan FX, OX, UX, XX, FI ni FR: Dolphin (y este
 * intérprete) no calcula el resultado inexacto. Tampoco CR1.FX ni CR1.OX.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpu_test.h"

#define ORACLE_FPU_PATH "tests/unit/data/oracle_fpu.bin"
#define CODE_ADDR       FT_CODE
#define DATA_ADDR       FT_DATA
#define ROW_BYTES       64
#define MAX_REPORTS     12

/* Banderas de la fila (ver tools/ppc-oracle/oracle_fpu.c) */
#define F_KIND_MASK 7u
#define F_LOW_WORD  0x10u
#define F_FULL      0x20u
#define F_NO_FPRF   0x40u
#define F_NO_C      0x80u
#define F_NO_FX     0x100u
#define F_SDENORM   0x200u

#define FPRF_C      0x00010000u

#define FPSCR_CMP_MASK (FPSCR_MASK & ~(FPSCR_FX | FPSCR_OX | FPSCR_UX | FPSCR_XX | FPSCR_FI | FPSCR_FR))
#define CR_CMP_MASK    0xF6FFFFFFu

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) << 32 | rd32(p + 4);
}

static uint8_t *load_oracle(const char *path, uint32_t *count)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = size > 12 ? malloc((size_t)size) : NULL;
    if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (!buf)
        return NULL;
    *count = rd32(buf + 8);
    if (memcmp(buf, "GCOF", 4) != 0 || rd32(buf + 4) != 1 || 12 + (long)*count * ROW_BYTES != size) {
        free(buf);
        return NULL;
    }
    return buf;
}

#define cpu ft_cpu
#define mem ft_mem

static void test_oracle_fpu(void)
{
    uint32_t count = 0, bad = 0;
    uint8_t *data = load_oracle(ORACLE_FPU_PATH, &count);
    CHECK(data != NULL);
    CHECK(ft_setup() == 0);

    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *row = data + 12 + (size_t)k * ROW_BYTES;
        const uint32_t inst = rd32(row), flags = rd32(row + 4);
        const uint32_t fpscr_in = rd32(row + 8), cr_in = rd32(row + 12);
        const uint64_t fa = rd64(row + 16), fb = rd64(row + 24), fc = rd64(row + 32);
        const uint64_t fd_in = rd64(row + 40), fd_out = rd64(row + 48);
        const uint32_t fpscr_out = rd32(row + 56), cr_out = rd32(row + 60);
        const uint32_t kind = flags & F_KIND_MASK;

        cpu.fpr[1][0] = fd_in;
        cpu.fpr[2][0] = fa;
        cpu.fpr[3][0] = fb;
        cpu.fpr[4][0] = fc;
        cpu.fpscr = fpscr_in;
        gekko_fpu_sync(&cpu);
        cpu.cr = cr_in;
        cpu.gpr[3] = DATA_ADDR;
        if (kind == 1 || kind == 2)
            gc_write64(&mem, DATA_ADDR, fd_in);         /* lfs/lfd: memoria de entrada */
        else
            gc_write64(&mem, DATA_ADDR, 0xDEADBEEFDEADBEEFull);
        ft_exec(inst);

        uint64_t got = kind >= 3 ? gc_read64(&mem, DATA_ADDR) : cpu.fpr[1][0];
        uint64_t want = fd_out;
        if (flags & F_LOW_WORD) {
            got &= 0xFFFFFFFFu;
            want &= 0xFFFFFFFFu;
        }
        uint32_t fmask = (flags & F_FULL) ? FPSCR_MASK : FPSCR_CMP_MASK;
        uint32_t cmask = (flags & F_FULL) ? 0xFFFFFFFFu : CR_CMP_MASK;
        if (flags & F_NO_FPRF)
            fmask &= ~FPSCR_FPRF;
        if (flags & F_NO_C)
            fmask &= ~FPRF_C;
        if (flags & F_NO_FX) {
            fmask &= ~FPSCR_FX;
            cmask &= ~0x08000000u;                      /* CR1.FX */
        }
        if ((flags & F_SDENORM) && ((cpu.fpscr ^ fpscr_out) & FPSCR_FPRF) == FPRF_C) {
            /* QEMU clasifica como doble ("normal"); acá debe salir subnormal */
            const uint32_t cls = (cpu.fpscr & FPSCR_FPRF) >> 12;
            if (cls == 0x14 || cls == 0x18)
                fmask &= ~FPRF_C;
        }
        if (got != want || ((cpu.fpscr ^ fpscr_out) & fmask) || ((cpu.cr ^ cr_out) & cmask) ||
            cpu.pc != CODE_ADDR + 4) {
            if (bad++ < MAX_REPORTS)
                fprintf(stderr,
                        "  oraculo: inst %08x a=%016llx b=%016llx c=%016llx fpscr=%08x -> "
                        "f1 %016llx/%016llx fpscr %08x/%08x cr %08x/%08x (obtenido/esperado)\n",
                        (unsigned)inst, (unsigned long long)fa, (unsigned long long)fb,
                        (unsigned long long)fc, (unsigned)fpscr_in, (unsigned long long)got,
                        (unsigned long long)want, (unsigned)cpu.fpscr, (unsigned)fpscr_out,
                        (unsigned)cpu.cr, (unsigned)cr_out);
        }
    }
    ft_teardown();
    free(data);
    if (bad)
        fprintf(stderr, "  oraculo: %u de %u casos distintos\n", (unsigned)bad, (unsigned)count);
    CHECK(bad == 0);
    CHECK(count > 20000);
}

/* ---- casos a mano ------------------------------------------------------------- */

#define F1 1
#define F2 2
#define F3 3
#define F4 4

#define FADD(d, a, b, rc)    enc_a(63, d, a, b, 0, 21, rc)
#define FADDS(d, a, b)       enc_a(59, d, a, b, 0, 21, 0)
#define FDIV(d, a, b)        enc_a(63, d, a, b, 0, 18, 0)
#define FMADD(d, a, c, b)    enc_a(63, d, a, b, c, 29, 0)
#define FMSUB(d, a, c, b)    enc_a(63, d, a, b, c, 28, 0)
#define FNMADD(d, a, c, b)   enc_a(63, d, a, b, c, 31, 0)
#define FNMSUB(d, a, c, b)   enc_a(63, d, a, b, c, 30, 0)
#define FMADDS(d, a, c, b)   enc_a(59, d, a, b, c, 29, 0)
#define FMSUBS(d, a, c, b)   enc_a(59, d, a, b, c, 28, 0)
#define FNMADDS(d, a, c, b)  enc_a(59, d, a, b, c, 31, 0)
#define FNMSUBS(d, a, c, b)  enc_a(59, d, a, b, c, 30, 0)
#define FRES(d, b)           enc_a(59, d, 0, b, 0, 24, 0)
#define FRSQRTE(d, b)        enc_a(63, d, 0, b, 0, 26, 0)
#define FRSP(d, b)           enc_x(63, d, 0, b, 12, 0)
#define FCTIW(d, b)          enc_x(63, d, 0, b, 14, 0)
#define MTFSF(fm, b)         (63u << 26 | (uint32_t)(fm) << 17 | (uint32_t)(b) << 11 | 711u << 1)
#define PS_ADD(d, a, b)      enc_a(4, d, a, b, 0, 21, 0)

static uint64_t run1(uint32_t inst, uint64_t a, uint64_t b, uint64_t c)
{
    ft_cpu.fpr[F2][0] = a;
    ft_cpu.fpr[F3][0] = b;
    ft_cpu.fpr[F4][0] = c;
    ft_exec(inst);
    return ft_cpu.fpr[F1][0];
}

/* Valores de las tablas del Gekko (calculados con el algoritmo de Dolphin,
 * que salió de pruebas en hardware) y los casos especiales. */
static void test_fres_frsqrte(void)
{
    CHECK(ft_setup() == 0);
    CHECK_EQ_U64(run1(FRES(F1, F3), 0, dbits(1.0), 0), 0x3FEFFF0000000000ull);   /* 0.99987793 */
    CHECK_EQ_U64(ft_cpu.fpr[F1][1], 0x3FEFFF0000000000ull);                       /* también ps1 */
    CHECK_EQ_U64(run1(FRES(F1, F3), 0, dbits(2.0), 0), 0x3FDFFF0000000000ull);
    CHECK_EQ_U64(run1(FRES(F1, F3), 0, dbits(-3.0), 0), 0xBFD5550000000000ull);
    CHECK_EQ_U64(run1(FRES(F1, F3), 0, dbits(1e300), 0), 0x0000000000000000ull);  /* grande: 0 */
    CHECK_EQ_U64(run1(FRES(F1, F3), 0, 0x37E0000000000000ull, 0), 0x47EFFFFFE0000000ull); /* FLT_MAX */
    CHECK_EQ_U64(run1(FRSQRTE(F1, F3), 0, dbits(1.0), 0), 0x3FEFFE8000000000ull);
    CHECK_EQ_U64(run1(FRSQRTE(F1, F3), 0, dbits(4.0), 0), 0x3FDFFE8000000000ull);
    CHECK_EQ_U64(run1(FRSQRTE(F1, F3), 0, dbits(2.0), 0), 0x3FE69FA000000000ull);
    CHECK_EQ_U64(run1(FRSQRTE(F1, F3), 0, 0x7FF0000000000000ull, 0), 0);          /* +inf -> 0 */
    CHECK(!(ft_cpu.fpscr & FPSCR_VX));

    ft_cpu.fpscr = 0;
    CHECK_EQ_U64(run1(FRES(F1, F3), 0, 0x8000000000000000ull, 0), 0xFFF0000000000000ull);
    CHECK(ft_cpu.fpscr & FPSCR_ZX);
    ft_cpu.fpscr = 0;
    CHECK_EQ_U64(run1(FRSQRTE(F1, F3), 0, dbits(-1.0), 0), 0x7FF8000000000000ull);
    CHECK((ft_cpu.fpscr & (FPSCR_VXSQRT | FPSCR_VX)) == (FPSCR_VXSQRT | FPSCR_VX));
    ft_cpu.fpscr = 0;
    CHECK_EQ_U64(run1(FRES(F1, F3), 0, 0x7FF0000000000001ull, 0), 0x7FF8000000000001ull);
    CHECK(ft_cpu.fpscr & FPSCR_VXSNAN);
    ft_teardown();
}

/* FPSCR[NI]: los resultados subnormales van a cero con su signo. */
static void test_non_ieee_mode(void)
{
    const uint64_t tiny = sbits(0x00000001);        /* el menor subnormal de simple */
    CHECK(ft_setup() == 0);
    CHECK_EQ_U64(run1(FADDS(F1, F2, F3), 0, tiny, 0), tiny);
    ft_cpu.fpscr = FPSCR_NI;
    CHECK_EQ_U64(run1(FADDS(F1, F2, F3), 0x8000000000000000ull, tiny | 0x8000000000000000ull, 0),
                 0x8000000000000000ull);
    CHECK_EQ_U64(run1(FADD(F1, F2, F3, 0), 0, 0x0000000000000001ull, 0), 0);
    CHECK_EQ_U64(run1(FRSP(F1, F3), 0, 0x380FFFFFF0000000ull, 0), 0);   /* redondearía a normal */
    ft_cpu.fpscr = 0;
    CHECK_EQ_U64(run1(FRSP(F1, F3), 0, 0x380FFFFFF0000000ull, 0), 0x3810000000000000ull);
    ft_teardown();
}

/* VE y ZE: el destino no cambia; FEX; con MSR[FE0], excepción de programa. */
static void test_enabled_exceptions(void)
{
    CHECK(ft_setup() == 0);
    ft_cpu.fpr[F1][0] = 0x1234;
    ft_cpu.fpscr = FPSCR_VE;
    run1(FDIV(F1, F2, F3), 0, 0, 0);                /* 0/0 */
    CHECK_EQ_U64(ft_cpu.fpr[F1][0], 0x1234);
    CHECK((ft_cpu.fpscr & (FPSCR_VXZDZ | FPSCR_VX | FPSCR_FEX | FPSCR_FX)) ==
          (FPSCR_VXZDZ | FPSCR_VX | FPSCR_FEX | FPSCR_FX));
    CHECK(ft_cpu.pc == FT_CODE + 4);                /* sin MSR[FE0|FE1]: no salta */

    ft_cpu.fpscr = FPSCR_ZE;
    ft_cpu.msr = MSR_FP | MSR_FE0;
    run1(FDIV(F1, F2, F3), dbits(1.0), 0, 0);       /* 1/0 */
    CHECK_EQ_U64(ft_cpu.fpr[F1][0], 0x1234);
    CHECK(ft_cpu.fpscr & FPSCR_ZX);
    CHECK(ft_cpu.pc == EXC_PROGRAM);
    CHECK(ft_cpu.spr[SPR_SRR0] == FT_CODE);
    CHECK(ft_cpu.spr[SPR_SRR1] & SRR1_PROGRAM_FP);

    /* ∞/0 no es división por cero (D-030) */
    ft_cpu.msr = MSR_FP;
    ft_cpu.fpscr = 0;
    CHECK_EQ_U64(run1(FDIV(F1, F2, F3), 0x7FF0000000000000ull, 0, 0), 0x7FF0000000000000ull);
    CHECK(!(ft_cpu.fpscr & FPSCR_ZX));

    /* ∞ × 0 + QNaN: el resultado es frB pero VXIMZ se prende (D-030) */
    ft_cpu.fpscr = 0;
    CHECK_EQ_U64(run1(FMADD(F1, F2, F4, F3), 0x7FF0000000000000ull, 0x7FF8000000000123ull, 0),
                 0x7FF8000000000123ull);
    CHECK(ft_cpu.fpscr & FPSCR_VXIMZ);

    /* mtfsf que deja una excepción habilitada: FEX y, con FE1, la trampa */
    ft_cpu.fpscr = FPSCR_ZX;
    ft_cpu.msr = MSR_FP | MSR_FE1;
    ft_cpu.fpr[F3][0] = FPSCR_ZX | FPSCR_ZE;
    ft_exec(MTFSF(0xFF, F3));
    CHECK(ft_cpu.fpscr & FPSCR_FEX);
    CHECK(ft_cpu.pc == EXC_PROGRAM);
    ft_teardown();
}

/* MSR[FP] = 0: aritmética, cargas y paired singles dan "FPU no disponible". */
static void test_fp_unavailable(void)
{
    static const uint32_t insts[] = {
        0xFC22182Au,                            /* fadd f1,f2,f3 */
        0xEC22182Au,                            /* fadds */
        0xC0230000u,                            /* lfs f1,0(r3) */
        0x7C20042Eu,                            /* lfsx f1,0,r0 */
        0x1022182Au,                            /* ps_add */
        0xE0230000u,                            /* psq_l f1,0(r3),0,0 */
    };
    CHECK(ft_setup() == 0);
    for (size_t k = 0; k < sizeof insts / sizeof insts[0]; k++) {
        ft_cpu.msr = 0;
        ft_cpu.gpr[3] = FT_DATA;
        ft_cpu.fpr[F1][0] = 0x55;
        ft_exec(insts[k]);
        CHECK(ft_cpu.pc == EXC_FP_UNAVAILABLE);
        CHECK(ft_cpu.spr[SPR_SRR0] == FT_CODE);
        CHECK_EQ_U64(ft_cpu.fpr[F1][0], 0x55);
    }
    ft_teardown();
}

static void test_loads_stores(void)
{
    CHECK(ft_setup() == 0);
    ft_cpu.gpr[3] = FT_DATA;
    ft_cpu.gpr[4] = 8;
    gc_write32(&ft_mem, FT_DATA, 0x3FC00000u);              /* 1.5f */
    gc_write64(&ft_mem, FT_DATA + 8, 0x400921FB54442D18ull); /* pi */

    ft_exec(enc_d(48, F1, 3, 0));                           /* lfs: ps0 y ps1 */
    CHECK_EQ_U64(ft_cpu.fpr[F1][0], dbits(1.5));
    CHECK_EQ_U64(ft_cpu.fpr[F1][1], dbits(1.5));
    ft_cpu.fpr[F1][1] = 7;
    ft_exec(enc_x(31, F1, 3, 4, 599, 0));                   /* lfdx: solo ps0 */
    CHECK_EQ_U64(ft_cpu.fpr[F1][0], 0x400921FB54442D18ull);
    CHECK_EQ_U64(ft_cpu.fpr[F1][1], 7);

    ft_exec(enc_d(53, F1, 3, 16));                          /* stfsu: actualiza r3 */
    CHECK(ft_cpu.gpr[3] == FT_DATA + 16);
    CHECK(gc_read32(&ft_mem, FT_DATA + 16) == 0x40490FDAu); /* pi truncado a simple */
    ft_exec(enc_x(31, F1, 3, 4, 759, 0));                   /* stfdux */
    CHECK(ft_cpu.gpr[3] == FT_DATA + 24);
    CHECK(gc_read64(&ft_mem, FT_DATA + 24) == 0x400921FB54442D18ull);
    ft_exec(enc_x(31, F1, 0, 3, 983, 0));                   /* stfiwx: la palabra baja */
    CHECK(gc_read32(&ft_mem, FT_DATA + 24) == 0x54442D18u);
    ft_exec(enc_d(51, F2, 3, -24));                         /* lfdu */
    CHECK(ft_cpu.gpr[3] == FT_DATA);
    CHECK_EQ_U64(ft_cpu.fpr[F2][0], (uint64_t)0x3FC00000u << 32);

    /* stfs de exponentes chicos: sin definir, como Dolphin (D-030) */
    ft_cpu.fpr[F1][0] = 0x800FFFFFFFFFFFFFull;
    ft_exec(enc_d(52, F1, 3, 0));
    CHECK(gc_read32(&ft_mem, FT_DATA) == 0x807FFFFFu);

    /* dirección no alineada a 4: excepción de alineación con DAR */
    ft_cpu.gpr[3] = FT_DATA + 2;
    ft_cpu.fpr[F1][0] = 0x99;
    ft_exec(enc_d(48, F1, 3, 0));
    CHECK(ft_cpu.pc == EXC_ALIGNMENT);
    CHECK(ft_cpu.spr[SPR_DAR] == FT_DATA + 2);
    CHECK_EQ_U64(ft_cpu.fpr[F1][0], 0x99);
    ft_teardown();
}

/* fctiw en los 4 modos; palabra alta 0xFFF80000 y el bit 32 con -0. */
static void test_fctiw_modes(void)
{
    static const uint32_t pos[4] = { 2, 2, 3, 2 }, neg[4] = { 0xFFFFFFFEu, 0xFFFFFFFEu, 0xFFFFFFFEu, 0xFFFFFFFDu };
    CHECK(ft_setup() == 0);
    for (uint32_t rn = 0; rn < 4; rn++) {
        ft_rn(rn);
        CHECK_EQ_U64(run1(FCTIW(F1, F3), 0, dbits(2.5), 0), 0xFFF8000000000000ull | pos[rn]);
        CHECK_EQ_U64(run1(FCTIW(F1, F3), 0, dbits(-2.5), 0), 0xFFF8000000000000ull | neg[rn]);
    }
    ft_rn(0);
    CHECK_EQ_U64(run1(FCTIW(F1, F3), 0, 0x8000000000000000ull, 0), 0xFFF8000100000000ull);
    ft_teardown();
}

/* fnmadd/fnmsub = -(fmadd/fmsub) también con redondeo dirigido: se redondea y
 * después se niega (el oráculo solo cubre RN = 0 y 1, D-030). */
static void test_fnmadd_directed(void)
{
    static const uint64_t v[] = {
        0x3FF0000000000001ull, 0xBFD5555555555555ull, 0x400921FB54442D18ull, 0x3E80000000000001ull,
        0xC1E0000000200000ull, 0x7FEFFFFFFFFFFFFFull, 0x0000000000000003ull, 0x3FB999999999999Aull,
    };
    static const uint32_t s[] = {
        0x3F800001u, 0xBEAAAAABu, 0x40490FDBu, 0x00000003u, 0x7F7FFFFFu, 0x3DCCCCCDu, 0xC0400000u, 0x33800000u,
    };
    const size_t n = sizeof v / sizeof v[0];
    uint32_t checked = 0;
    CHECK(ft_setup() == 0);
    for (uint32_t rn = 2; rn < 4; rn++) {
        ft_rn(rn);
        for (size_t a = 0; a < n; a++)
            for (size_t b = 0; b < n; b++)
                for (size_t c = 0; c < n; c += 3) {
                    const uint64_t md = run1(FMADD(F1, F2, F4, F3), v[a], v[b], v[c]);
                    const uint64_t nmd = run1(FNMADD(F1, F2, F4, F3), v[a], v[b], v[c]);
                    const uint64_t ms = run1(FMSUB(F1, F2, F4, F3), v[a], v[b], v[c]);
                    const uint64_t nms = run1(FNMSUB(F1, F2, F4, F3), v[a], v[b], v[c]);
                    CHECK_EQ_U64(nmd, md ^ 0x8000000000000000ull);
                    CHECK_EQ_U64(nms, ms ^ 0x8000000000000000ull);
                    const uint64_t sa = sbits(s[a]), sb = sbits(s[b]), sc = sbits(s[c]);
                    CHECK_EQ_U64(run1(FNMADDS(F1, F2, F4, F3), sa, sb, sc),
                                 run1(FMADDS(F1, F2, F4, F3), sa, sb, sc) ^ 0x8000000000000000ull);
                    CHECK_EQ_U64(run1(FNMSUBS(F1, F2, F4, F3), sa, sb, sc),
                                 run1(FMSUBS(F1, F2, F4, F3), sa, sb, sc) ^ 0x8000000000000000ull);
                    checked += 4;
                }
    }
    CHECK(checked > 200);
    ft_teardown();
}

/* fmuls y fmadds redondean frC a 25 bits antes de multiplicar (Gekko):
 * (1 + 2^-23) * (1 + 2^-25) daría 1 + 2^-23; con frC = 1 + 2^-24, 1 + 2^-22. */
static void test_force25bit(void)
{
    const uint64_t a = 0x3FF0000020000000ull, c = 0x3FF0000008000000ull;
    CHECK(ft_setup() == 0);
    CHECK_EQ_U64(run1(enc_a(59, F1, F2, 0, F4, 25, 0), a, 0, c), 0x3FF0000040000000ull);    /* fmuls */
    CHECK_EQ_U64(run1(enc_a(63, F1, F2, 0, F4, 25, 0), a, 0, c), 0x3FF0000028000010ull);    /* fmul: sin redondear frC */
    CHECK_EQ_U64(run1(FMADDS(F1, F2, F4, F3), a, 0, c), 0x3FF0000040000000ull);
    ft_teardown();
}

/* gekko_run corre con el redondeo de FPSCR y al volver deja el más cercano. */
static void test_run_rounding(void)
{
    volatile double one = 1.0, tiny = 0x1p-60;
    CHECK(ft_setup() == 0);
    gc_write32(&ft_mem, FT_CODE, FADD(F1, F2, F3, 0));
    ft_cpu.pc = FT_CODE;
    ft_cpu.fpscr = 2;                                       /* hacia +inf */
    ft_cpu.fpr[F2][0] = dbits(1.0);
    ft_cpu.fpr[F3][0] = dbits(0x1p-60);
    CHECK(gekko_run(&ft_cpu, 1) == 1);
    CHECK_EQ_U64(ft_cpu.fpr[F1][0], 0x3FF0000000000001ull);
    CHECK(one + tiny == 1.0);                               /* la máquina, al más cercano */
    ft_teardown();
}

const test_case_t cpu_fpu_tests[] = {
    { "gekko: FPU escalar contra el oraculo (750CXe en QEMU)", test_oracle_fpu },
    { "gekko: fres y frsqrte con las tablas del Gekko", test_fres_frsqrte },
    { "gekko: FPSCR[NI] lleva los subnormales a cero", test_non_ieee_mode },
    { "gekko: VE, ZE, FEX y la excepcion de programa", test_enabled_exceptions },
    { "gekko: MSR[FP] = 0 da FPU no disponible", test_fp_unavailable },
    { "gekko: lfs, lfd, stfs, stfd, stfiwx y alineacion", test_loads_stores },
    { "gekko: fctiw en los cuatro modos de redondeo", test_fctiw_modes },
    { "gekko: fnmadd y fnmsub con redondeo dirigido", test_fnmadd_directed },
    { "gekko: fmuls y fmadds redondean frC a 25 bits", test_force25bit },
    { "gekko: gekko_run y el modo de redondeo de la maquina", test_run_rounding },
    { NULL, NULL },
};
