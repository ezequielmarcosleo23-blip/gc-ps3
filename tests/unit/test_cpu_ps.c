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
 * Paired singles y psq_l/psq_st. El 750CXe no los tiene, así que no hay
 * oráculo directo: cada mitad se compara con la instrucción escalar de
 * precisión simple equivalente (ya verificada contra el oráculo), en los 4
 * modos de redondeo. La cuantización y las habilitaciones de HID2, a mano.
 */
#include "fpu_test.h"

#define cpu ft_cpu
#define mem ft_mem

#define F1 1u
#define F2 2u
#define F3 3u
#define F4 4u

/* ---- equivalencia con las escalares ----------------------------------------- */

enum { K_SAME, K_MULS0, K_MULS1, K_MADDS0, K_MADDS1, K_SUM0, K_SUM1, K_RSQRTE };

typedef struct ps_case {
    const char *name;
    uint32_t    ps;         /* instrucción paired (f1 = f(f2, f3, f4)) */
    uint32_t    scalar;     /* equivalente escalar sobre ps0 */
    int         kind;
} ps_case_t;

/* Constantes (van en una tabla estática) */
#define SCA(op, xo)   ((uint32_t)(op) << 26 | F1 << 21 | F2 << 16 | F3 << 11 | F4 << 6 | (uint32_t)(xo) << 1)
#define PSA(xo)       SCA(4, xo)
#define FADDS         SCA(59, 21)
#define FRSP          enc_x(63, F1, 0, F3, 12, 0)
#define FRSQRTE       enc_a(63, F1, 0, F3, 0, 26, 0)

static const ps_case_t cases[] = {
    { "ps_add", PSA(21), SCA(59, 21), K_SAME },     { "ps_sub", PSA(20), SCA(59, 20), K_SAME },
    { "ps_div", PSA(18), SCA(59, 18), K_SAME },     { "ps_mul", PSA(25), SCA(59, 25), K_SAME },
    { "ps_madd", PSA(29), SCA(59, 29), K_SAME },    { "ps_msub", PSA(28), SCA(59, 28), K_SAME },
    { "ps_nmadd", PSA(31), SCA(59, 31), K_SAME },   { "ps_nmsub", PSA(30), SCA(59, 30), K_SAME },
    { "ps_sel", PSA(23), SCA(63, 23), K_SAME },     { "ps_res", PSA(24), SCA(59, 24), K_SAME },
    { "ps_muls0", PSA(12), SCA(59, 25), K_MULS0 },  { "ps_muls1", PSA(13), SCA(59, 25), K_MULS1 },
    { "ps_madds0", PSA(14), SCA(59, 29), K_MADDS0 }, { "ps_madds1", PSA(15), SCA(59, 29), K_MADDS1 },
    { "ps_sum0", PSA(10), 0, K_SUM0 },              { "ps_sum1", PSA(11), 0, K_SUM1 },
    { "ps_rsqrte", PSA(26), 0, K_RSQRTE },
};

/* Además de simples: dobles con bits de más en la mantisa (frC pasa por
 * Force25Bit en ps_mul, ps_muls*, ps_madd...) */
static const uint64_t dextra[] = {
    0x3FF0000008000000ull, 0x3FF0000020000000ull, 0xBFD5555555555555ull, 0x400921FB54442D18ull,
    0x3E80000000000001ull, 0x47EFFFFFF0000000ull, 0x380FFFFFF0000000ull,
};

static const uint32_t svals[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807FFFFF, 0x3F800000, 0xBFC00000, 0x7F7FFFFF,
    0x7F800000, 0xFF800000, 0x7F800001, 0x7FC00000, 0x3DCCCCCD, 0x40490FDB, 0xC0400000,
    0x4B7FFFFF, 0x33800000, 0x42480000, 0xBC88CC38, 0x1B1C72A0,
};

typedef struct scalar_out {
    uint64_t value;
    uint32_t fpscr;
} scalar_out_t;

/* Una escalar con f2 = a, f3 = b, f4 = c y FPSCR = rn. */
static scalar_out_t scalar(uint32_t inst, uint32_t rn, uint64_t a, uint64_t b, uint64_t c)
{
    scalar_out_t o;
    cpu.fpscr = rn;
    gekko_fpu_sync(&cpu);
    cpu.fpr[F2][0] = a;
    cpu.fpr[F3][0] = b;
    cpu.fpr[F4][0] = c;
    ft_exec(inst);
    o.value = cpu.fpr[F1][0];
    o.fpscr = cpu.fpscr;
    return o;
}

/* Lo que la paired debe dar en cada mitad, armado con escalares. */
static void expected(const ps_case_t *pc, uint32_t rn, const uint64_t a[2], const uint64_t b[2],
                     const uint64_t c[2], scalar_out_t out[2])
{
    for (int h = 0; h < 2; h++) {
        switch (pc->kind) {
        case K_SAME:   out[h] = scalar(pc->scalar, rn, a[h], b[h], c[h]); break;
        case K_MULS0:  out[h] = scalar(pc->scalar, rn, a[h], b[h], c[0]); break;
        case K_MULS1:  out[h] = scalar(pc->scalar, rn, a[h], b[h], c[1]); break;
        case K_MADDS0: out[h] = scalar(pc->scalar, rn, a[h], b[h], c[0]); break;
        case K_MADDS1: out[h] = scalar(pc->scalar, rn, a[h], b[h], c[1]); break;
        case K_SUM0:
            out[h] = h == 0 ? scalar(FADDS, rn, a[0], b[1], 0) : scalar(FRSP, rn, 0, c[1], 0);
            break;
        case K_SUM1:
            out[h] = h == 0 ? scalar(FRSP, rn, 0, c[0], 0) : scalar(FADDS, rn, a[0], b[1], 0);
            break;
        case K_RSQRTE: {
            const scalar_out_t est = scalar(FRSQRTE, rn, 0, b[h], 0);
            out[h] = scalar(FRSP, rn, 0, est.value, 0);
            break;
        }
        }
    }
}

static uint32_t lcg_state = 0x1234567u;
static uint32_t pick(uint32_t n)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (lcg_state >> 8) % n;
}

static void test_ps_vs_scalar(void)
{
    const uint32_t n = sizeof svals / sizeof svals[0];
    const uint32_t ignore = FPSCR_FI | FPSCR_FR | FPSCR_FPRF;
    uint32_t bad = 0, total = 0;
    CHECK(ft_setup() == 0);

    for (uint32_t t = 0; t < 500; t++) {
        uint64_t a[2], b[2], c[2];
        const uint32_t nx = sizeof dextra / sizeof dextra[0];
        for (int h = 0; h < 2; h++) {
            a[h] = sbits(svals[pick(n)]);
            b[h] = sbits(svals[pick(n)]);
            c[h] = (t & 1) ? dextra[pick(nx)] : sbits(svals[pick(n)]);
        }
        const uint32_t rn = t % 4;
        for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            const ps_case_t *pc = &cases[k];
            scalar_out_t want[2];
            expected(pc, rn, a, b, c, want);

            cpu.fpscr = rn;
            gekko_fpu_sync(&cpu);
            cpu.fpr[F2][0] = a[0]; cpu.fpr[F2][1] = a[1];
            cpu.fpr[F3][0] = b[0]; cpu.fpr[F3][1] = b[1];
            cpu.fpr[F4][0] = c[0]; cpu.fpr[F4][1] = c[1];
            ft_exec(pc->ps);
            total++;

            /* FPRF de ps0 (de ps1 en ps_sum1); excepciones: las de las dos
             * mitades (salvo ps_sum y ps_rsqrte, que no las hacen en frsp) */
            const int fprf_half = pc->kind == K_SUM1 ? 1 : 0;
            int ok = cpu.fpr[F1][0] == want[0].value && cpu.fpr[F1][1] == want[1].value &&
                     (cpu.fpscr & FPSCR_FPRF) == (want[fprf_half].fpscr & FPSCR_FPRF);
            if (pc->kind != K_SUM0 && pc->kind != K_SUM1 && pc->kind != K_RSQRTE)
                ok = ok && (cpu.fpscr & ~ignore) == ((want[0].fpscr | want[1].fpscr) & ~ignore);
            if (!ok && bad++ < 8)
                fprintf(stderr,
                        "  %s rn=%u a=%016llx,%016llx b=%016llx,%016llx c=%016llx,%016llx -> "
                        "%016llx,%016llx fpscr %08x; esperado %016llx,%016llx fpscr %08x|%08x\n",
                        pc->name, (unsigned)rn, (unsigned long long)a[0], (unsigned long long)a[1],
                        (unsigned long long)b[0], (unsigned long long)b[1], (unsigned long long)c[0],
                        (unsigned long long)c[1], (unsigned long long)cpu.fpr[F1][0],
                        (unsigned long long)cpu.fpr[F1][1], (unsigned)cpu.fpscr,
                        (unsigned long long)want[0].value, (unsigned long long)want[1].value,
                        (unsigned)want[0].fpscr, (unsigned)want[1].fpscr);
        }
    }
    ft_teardown();
    if (bad)
        fprintf(stderr, "  %u de %u distintas\n", (unsigned)bad, (unsigned)total);
    CHECK(bad == 0);
}

/* ---- movimientos, mezclas y comparaciones ---------------------------------- */

#define PSX(d, a, b, xo, rc) enc_x(4, d, a, b, xo, rc)

static void test_moves_merges_compares(void)
{
    const uint64_t A0 = 0x7FF0000000000001ull, A1 = 0xC000000000000000ull;    /* SNaN, -2 */
    const uint64_t B0 = 0x3FF0000000000000ull, B1 = 0xFFF8000000000123ull;    /* 1, -QNaN */
    CHECK(ft_setup() == 0);

#define LOAD_AB() (cpu.fpr[F2][0] = A0, cpu.fpr[F2][1] = A1, cpu.fpr[F3][0] = B0, cpu.fpr[F3][1] = B1)
    LOAD_AB();
    ft_exec(PSX(F1, F2, F3, 528, 0));                       /* ps_merge00 */
    CHECK(cpu.fpr[F1][0] == A0 && cpu.fpr[F1][1] == B0);
    ft_exec(PSX(F1, F2, F3, 560, 0));                       /* ps_merge01 */
    CHECK(cpu.fpr[F1][0] == A0 && cpu.fpr[F1][1] == B1);
    ft_exec(PSX(F1, F2, F3, 624, 0));                       /* ps_merge11 */
    CHECK(cpu.fpr[F1][0] == A1 && cpu.fpr[F1][1] == B1);
    ft_exec(PSX(F2, F2, F3, 592, 0));                       /* ps_merge10 f2,f2,f3 (alias) */
    CHECK(cpu.fpr[F2][0] == A1 && cpu.fpr[F2][1] == B0);

    LOAD_AB();
    ft_exec(PSX(F1, 0, F2, 40, 0));                         /* ps_neg: la SNaN no cambia */
    CHECK(cpu.fpr[F1][0] == (A0 ^ 0x8000000000000000ull) && cpu.fpr[F1][1] == 0x4000000000000000ull);
    ft_exec(PSX(F1, 0, F3, 264, 0));                        /* ps_abs */
    CHECK(cpu.fpr[F1][0] == B0 && cpu.fpr[F1][1] == 0x7FF8000000000123ull);
    ft_exec(PSX(F1, 0, F3, 136, 0));                        /* ps_nabs */
    CHECK(cpu.fpr[F1][0] == 0xBFF0000000000000ull && cpu.fpr[F1][1] == B1);
    ft_exec(PSX(F1, 0, F2, 72, 0));                         /* ps_mr */
    CHECK(cpu.fpr[F1][0] == A0 && cpu.fpr[F1][1] == A1);
    CHECK(cpu.fpscr == 0);                                  /* binarias: FPSCR igual */

    /* ps_cmpu0 cr2: -2 (A1 no; ps0 = SNaN)... se usan valores comunes */
    cpu.fpr[F2][0] = dbits(1.0);
    cpu.fpr[F2][1] = dbits(5.0);
    cpu.fpr[F3][0] = dbits(2.0);
    cpu.fpr[F3][1] = B1;
    ft_exec(PSX(2u << 2, F2, F3, 0, 0));                    /* ps_cmpu0 cr2: 1 < 2 */
    CHECK(((cpu.cr >> 20) & 0xF) == 8);
    CHECK((cpu.fpscr & FPSCR_FPCC) == 0x8000u);
    ft_exec(PSX(5u << 2, F2, F3, 96, 0));                   /* ps_cmpo1 cr5: con QNaN */
    CHECK(((cpu.cr >> 8) & 0xF) == 1);
    CHECK(cpu.fpscr & FPSCR_VXVC);

    /* Rc = 1: CR1 = FX, FEX, VX, OX */
    cpu.fpscr = 0;
    cpu.fpr[F2][0] = A0;
    cpu.fpr[F2][1] = dbits(1.0);
    cpu.fpr[F3][0] = cpu.fpr[F3][1] = dbits(1.0);
    ft_exec(enc_a(4, F1, F2, F3, 0, 21, 1));                /* ps_add. con SNaN */
    CHECK(((cpu.cr >> 24) & 0xF) == 0xA);
    ft_teardown();
#undef LOAD_AB
}

/* ---- psq_l / psq_st ---------------------------------------------------------- */

static uint32_t gqr(uint32_t ld_type, uint32_t ld_scale, uint32_t st_type, uint32_t st_scale)
{
    return ld_scale << 24 | ld_type << 16 | st_scale << 8 | st_type;
}

#define PSQ_L(d, a, w, i, disp)   enc_psq(56, d, a, w, i, disp)
#define PSQ_LU(d, a, w, i, disp)  enc_psq(57, d, a, w, i, disp)
#define PSQ_ST(s, a, w, i, disp)  enc_psq(60, s, a, w, i, disp)
#define PSQ_STU(s, a, w, i, disp) enc_psq(61, s, a, w, i, disp)

static void set_pair(uint32_t r, double v0, double v1)
{
    cpu.fpr[r][0] = dbits(v0);
    cpu.fpr[r][1] = dbits(v1);
}

static void test_quantized_store(void)
{
    CHECK(ft_setup() == 0);
    cpu.gpr[3] = FT_DATA;

    /* GQR0 = float: dos simples; los subnormales de simple van a cero */
    cpu.spr[SPR_GQR0] = 0;
    cpu.fpr[F1][0] = dbits(1.5);
    cpu.fpr[F1][1] = 0xB800000000000000ull;                     /* -2^-127 */
    ft_exec(PSQ_ST(F1, 3, 0, 0, 0));
    CHECK(gc_read64(&mem, FT_DATA) == 0x3FC0000080000000ull);
    gc_write64(&mem, FT_DATA, 0x1111111122222222ull);
    ft_exec(PSQ_ST(F1, 3, 1, 0, 0));                            /* W = 1: solo ps0 */
    CHECK(gc_read64(&mem, FT_DATA) == 0x3FC0000022222222ull);

    cpu.spr[SPR_GQR0 + 1] = gqr(0, 0, GQR_U8, 0);
    set_pair(F1, 1.5, 300.0);
    ft_exec(PSQ_ST(F1, 3, 0, 1, 0));                            /* trunca y satura */
    CHECK(gc_read16(&mem, FT_DATA) == 0x01FF);
    set_pair(F1, -5.0, 0.9);
    ft_exec(PSQ_ST(F1, 3, 0, 1, 0));
    CHECK(gc_read16(&mem, FT_DATA) == 0x0000);

    cpu.spr[SPR_GQR0 + 2] = gqr(0, 0, GQR_S8, 0);
    set_pair(F1, -5.7, 200.0);
    ft_exec(PSQ_ST(F1, 3, 0, 2, 0));
    CHECK(gc_read16(&mem, FT_DATA) == 0xFB7F);
    cpu.fpr[F1][0] = 0x7FF8000000000000ull;                     /* NaN -> 0 */
    cpu.fpr[F1][1] = dbits(-200.0);
    ft_exec(PSQ_ST(F1, 3, 0, 2, 0));
    CHECK(gc_read16(&mem, FT_DATA) == 0x0080);

    cpu.spr[SPR_GQR0 + 3] = gqr(0, 0, GQR_U16, 8);              /* x 256 */
    set_pair(F1, 1.5, 0.25);
    ft_exec(PSQ_ST(F1, 3, 0, 3, 0));
    CHECK(gc_read32(&mem, FT_DATA) == 0x01800040u);

    cpu.spr[SPR_GQR0 + 4] = gqr(0, 0, GQR_S16, 63);             /* escala -1: / 2 */
    set_pair(F1, 100.0, -7.0);
    ft_exec(PSQ_ST(F1, 3, 0, 4, 0));
    CHECK(gc_read32(&mem, FT_DATA) == 0x0032FFFDu);
    gc_write32(&mem, FT_DATA + 4, 0xAAAAAAAAu);
    ft_exec(PSQ_ST(F1, 3, 1, 4, 4));                            /* W = 1 en FT_DATA + 4 */
    CHECK(gc_read32(&mem, FT_DATA + 4) == 0x0032AAAAu);

    /* psq_stu con desplazamiento negativo (12 bits con signo) */
    cpu.gpr[5] = FT_DATA + 0x810;
    ft_exec(PSQ_STU(F1, 5, 1, 4, -0x800));
    CHECK(cpu.gpr[5] == FT_DATA + 0x10);
    CHECK(gc_read16(&mem, FT_DATA + 0x10) == 0x0032);
    ft_teardown();
}

static void test_quantized_load(void)
{
    CHECK(ft_setup() == 0);
    cpu.gpr[3] = FT_DATA;

    gc_write64(&mem, FT_DATA, 0x40490FDB00000001ull);
    cpu.spr[SPR_GQR0] = 0;
    ft_exec(PSQ_L(F1, 3, 0, 0, 0));                             /* float: como lfs */
    CHECK(cpu.fpr[F1][0] == sbits(0x40490FDBu));
    CHECK(cpu.fpr[F1][1] == 0x36A0000000000000ull);             /* 2^-149, subnormal de simple */
    ft_exec(PSQ_L(F1, 3, 1, 0, 0));                             /* W = 1: ps1 = 1.0 */
    CHECK(cpu.fpr[F1][1] == dbits(1.0));

    gc_write16(&mem, FT_DATA, 0x10FF);
    cpu.spr[SPR_GQR0 + 1] = gqr(GQR_U8, 1, 0, 0);               /* / 2 */
    ft_exec(PSQ_L(F1, 3, 0, 1, 0));
    CHECK(cpu.fpr[F1][0] == dbits(8.0) && cpu.fpr[F1][1] == dbits(127.5));

    gc_write16(&mem, FT_DATA, 0x807F);
    cpu.spr[SPR_GQR0 + 2] = gqr(GQR_S8, 0, 0, 0);
    ft_exec(PSQ_L(F1, 3, 0, 2, 0));
    CHECK(cpu.fpr[F1][0] == dbits(-128.0) && cpu.fpr[F1][1] == dbits(127.0));

    gc_write32(&mem, FT_DATA, 0x0001FFFFu);
    cpu.spr[SPR_GQR0 + 3] = gqr(GQR_S16, 32, 0, 0);             /* escala -32: x 2^32 */
    ft_exec(PSQ_L(F1, 3, 0, 3, 0));
    CHECK(cpu.fpr[F1][0] == dbits(4294967296.0) && cpu.fpr[F1][1] == dbits(-4294967296.0));

    cpu.spr[SPR_GQR0 + 4] = gqr(GQR_U16, 0, 0, 0);
    gc_write16(&mem, FT_DATA + 2, 0xFFFF);
    ft_exec(PSQ_L(F1, 3, 1, 4, 2));                             /* W = 1 */
    CHECK(cpu.fpr[F1][0] == dbits(65535.0) && cpu.fpr[F1][1] == dbits(1.0));

    cpu.spr[SPR_GQR0 + 5] = gqr(1, 0, 0, 0);                    /* tipo sin definir: 0, 0 */
    ft_exec(PSQ_L(F1, 3, 0, 5, 0));
    CHECK(cpu.fpr[F1][0] == 0 && cpu.fpr[F1][1] == 0);

    /* psq_lu, psq_stx y psq_lux (I y W de la forma X) */
    cpu.gpr[4] = FT_DATA + 8;
    cpu.gpr[6] = 8;
    cpu.spr[SPR_GQR0 + 6] = gqr(GQR_FLOAT, 0, GQR_S16, 0);
    gc_write64(&mem, FT_DATA, 0x3F80000040000000ull);           /* 1.0, 2.0 */
    gc_write64(&mem, FT_DATA + 8, 0);
    ft_exec(PSQ_LU(F2, 4, 0, 0, -8));
    CHECK(cpu.gpr[4] == FT_DATA);
    CHECK(cpu.fpr[F2][0] == dbits(1.0) && cpu.fpr[F2][1] == dbits(2.0));
    ft_exec(enc_psqx(F2, 3, 6, 1, 6, 7));                        /* psq_stx W=1 I=6: s16 */
    CHECK(gc_read32(&mem, FT_DATA + 8) == 0x00010000u);
    ft_exec(enc_psqx(F3, 4, 6, 0, 6, 38));                       /* psq_lux I=6: float */
    CHECK(cpu.gpr[4] == FT_DATA + 8);
    CHECK(cpu.fpr[F3][0] == sbits(0x00010000u) && cpu.fpr[F3][1] == 0);
    ft_teardown();
}

/* ---- HID2 y MSR -------------------------------------------------------------- */

static void test_hid2_gating(void)
{
    const uint32_t ps_add = enc_a(4, F1, F2, F3, 0, 21, 0);
    const uint32_t psq_l = PSQ_L(F1, 3, 0, 0, 0);
    const uint32_t psq_lx = enc_psqx(F1, 0, 3, 0, 0, 6);
    const uint32_t dcbz_l = enc_x(4, 0, 0, 3, 1014, 0);
    CHECK(ft_setup() == 0);
    cpu.gpr[3] = FT_DATA;
    cpu.spr[SPR_GQR0] = 0;

    cpu.spr[SPR_HID2] = 0;                                      /* sin PSE: ilegales */
    ft_exec(ps_add);
    CHECK(cpu.pc == EXC_PROGRAM && (cpu.spr[SPR_SRR1] & SRR1_PROGRAM_ILLEGAL));
    ft_exec(psq_l);
    CHECK(cpu.pc == EXC_PROGRAM);
    ft_exec(dcbz_l);                                            /* dcbz_l no depende de PSE */
    CHECK(cpu.pc == FT_CODE + 4);
    cpu.msr = 0;                                                /* ilegal antes que FP */
    ft_exec(ps_add);
    CHECK(cpu.pc == EXC_PROGRAM);

    cpu.msr = MSR_FP;
    cpu.spr[SPR_HID2] = HID2_PSE;                               /* PSE sin LSQE */
    ft_exec(psq_l);
    CHECK(cpu.pc == EXC_PROGRAM);
    cpu.msr = MSR_FP;                                           /* la excepción apagó MSR[FP] */
    ft_exec(psq_lx);                                            /* las formas X no piden LSQE */
    CHECK(cpu.pc == FT_CODE + 4);
    ft_exec(ps_add);
    CHECK(cpu.pc == FT_CODE + 4);
    ft_teardown();
}

const test_case_t cpu_ps_tests[] = {
    { "gekko: paired singles = escalares por mitades (4 modos)", test_ps_vs_scalar },
    { "gekko: ps_merge, ps_neg/abs/mr, ps_cmp y Rc", test_moves_merges_compares },
    { "gekko: psq_st con GQR (float, u8, s8, u16, s16, W)", test_quantized_store },
    { "gekko: psq_l con GQR y las formas u y x", test_quantized_load },
    { "gekko: HID2[PSE], HID2[LSQE] y MSR[FP]", test_hid2_gating },
    { NULL, NULL },
};
