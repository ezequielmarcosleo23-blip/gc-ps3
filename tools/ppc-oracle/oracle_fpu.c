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
 * Oráculo de la FPU escalar: ejecuta cada instrucción en un PowerPC 750CXe
 * (QEMU, `make oracle`) y guarda entradas y salidas en
 * tests/unit/data/oracle_fpu.bin, que compara tests/unit/test_cpu_fpu.c.
 *
 * Registros: f1 = destino, f2 = frA, f3 = frB, f4 = frC; r3 apunta a 8 bytes
 * de memoria para lfs/lfd/stfs/stfd/stfiwx. FPSCR y CR de entrada se cargan
 * antes de la instrucción y se leen después; el FPSCR de entrada que se guarda
 * es el que quedó (mtfsf recalcula VX y FEX).
 *
 * Donde QEMU no sigue a la arquitectura, o la arquitectura no define el
 * resultado, la fila lo marca y la prueba no compara ese bit (D-030):
 *   fctiw/fctiwz  FPRF sin definir (Dolphin, por pruebas en hardware: no cambia)
 *   fcmpo         QEMU pone FPRF[C] con VXVC; la arquitectura solo cambia FPCC
 *   mtfsb1        QEMU no pone FX al prender un bit de excepción
 *   mffs          la palabra alta de frD está sin definir
 *   simple        QEMU clasifica en FPRF el resultado como doble: un subnormal
 *                 de simple le sale "normal"
 *   fnmadd/fnmsub QEMU niega antes de redondear y la arquitectura después: solo
 *                 se usan los modos simétricos (RN = 0 y 1)
 *   stfs          exponentes menores que 874: sin definir, no se generan
 *
 * Fuera del oráculo (no son del 750CXe o QEMU no los imita): fres y frsqrte
 * (tablas del Gekko), paired singles y psq_*, Force25Bit de frC (las de
 * precisión simple reciben solo valores de simple) y FPSCR[NI].
 *
 * Archivo: "GCOF", versión 1, cantidad de filas y filas de 16 palabras
 * big-endian (ver row_t).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Palabras de io[] que lee y escribe el código de prueba */
enum {
    IO_INST = 0, IO_FA = 2, IO_FB = 4, IO_FC = 6, IO_FD = 8, IO_FPSCR = 10, IO_CR = 12,
    IO_FD_OUT = 14, IO_FPSCR_OUT = 16, IO_CR_OUT = 18, IO_MEM = 20, IO_WORDS = 24,
};

typedef void (*test_fn)(uint32_t *io);

#define T(fname, text)                                                              \
    static void fname(uint32_t *io)                                                 \
    {                                                                               \
        __asm__ volatile("lfd 2,8(%0)\n\t"                                          \
                         "lfd 3,16(%0)\n\t"                                         \
                         "lfd 4,24(%0)\n\t"                                         \
                         "lfd 1,32(%0)\n\t"                                         \
                         "lfd 0,40(%0)\n\t"                                         \
                         "mtfsf 0xFF,0\n\t"                                         \
                         "mffs 0\n\t"                                               \
                         "stfd 0,40(%0)\n\t"                                        \
                         "lwz 6,48(%0)\n\t"                                         \
                         "mtcr 6\n\t"                                               \
                         "addi 3,%0,80\n\t"                                         \
                         "0: " text "\n\t"                                          \
                         "stfd 1,56(%0)\n\t"                                        \
                         "mffs 0\n\t"                                               \
                         "stfd 0,64(%0)\n\t"                                        \
                         "mfcr 6\n\t"                                               \
                         "stw 6,72(%0)\n\t"                                         \
                         "lis 6,0b@ha\n\t"                                          \
                         "addi 6,6,0b@l\n\t"                                        \
                         "lwz 6,0(6)\n\t"                                           \
                         "stw 6,0(%0)\n\t"                                          \
                         :                                                          \
                         : "b"(io)                                                  \
                         : "r3", "r6", "fr0", "fr1", "fr2", "fr3", "fr4", "cr0",    \
                           "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7", "memory"); \
    }

/* Formas: 2 = frA, frB; M = frA, frC; 3 = frA, frC, frB; 1 = frB */
#define A2(op) T(t_##op, #op " 1,2,3")
#define AM(op) T(t_##op, #op " 1,2,4")
#define A3(op) T(t_##op, #op " 1,2,4,3")
#define X1(op) T(t_##op, #op " 1,3")

A2(fadd) A2(fsub) AM(fmul) A2(fdiv) A3(fmadd) A3(fmsub) A3(fnmadd) A3(fnmsub) A3(fsel)
A2(fadds) A2(fsubs) AM(fmuls) A2(fdivs) A3(fmadds) A3(fmsubs) A3(fnmadds) A3(fnmsubs)
X1(frsp) X1(fctiw) X1(fctiwz) X1(fneg) X1(fabs) X1(fnabs) X1(fmr)
T(t_fadd_rc, "fadd. 1,2,3")   T(t_fmuls_rc, "fmuls. 1,2,4") T(t_fmadd_rc, "fmadd. 1,2,4,3")
T(t_frsp_rc, "frsp. 1,3")     T(t_fctiw_rc, "fctiw. 1,3")   T(t_fneg_rc, "fneg. 1,3")
T(t_fcmpu, "fcmpu 3,2,3")     T(t_fcmpo, "fcmpo 5,2,3")     T(t_fcmpu0, "fcmpu 0,2,3")
T(t_lfs, "lfs 1,0(3)")        T(t_lfd, "lfd 1,0(3)")
T(t_stfs, "stfs 2,0(3)")      T(t_stfd, "stfd 2,0(3)")      T(t_stfiwx, "stfiwx 2,0,3")
T(t_mffs, "mffs 1")           T(t_mffs_rc, "mffs. 1")
T(t_mtfsf_ff, "mtfsf 0xFF,3") T(t_mtfsf_01, "mtfsf 0x01,3") T(t_mtfsf_80, "mtfsf 0x80,3")
T(t_mtfsf_7e, "mtfsf. 0x7E,3")
T(t_mtfsfi_0, "mtfsfi 0,9")   T(t_mtfsfi_1, "mtfsfi 1,15")  T(t_mtfsfi_6, "mtfsfi 6,0")
T(t_mtfsfi_7, "mtfsfi. 7,3")
T(t_mtfsb0_0, "mtfsb0 0")     T(t_mtfsb0_7, "mtfsb0 7")     T(t_mtfsb0_30, "mtfsb0. 30")
T(t_mtfsb1_3, "mtfsb1 3")     T(t_mtfsb1_7, "mtfsb1 7")     T(t_mtfsb1_21, "mtfsb1 21")
T(t_mtfsb1_31, "mtfsb1. 31")  T(t_mtfsb1_1, "mtfsb1 1")
T(t_mcrfs_0, "mcrfs 2,0")     T(t_mcrfs_1, "mcrfs 4,1")     T(t_mcrfs_3, "mcrfs 7,3")
T(t_mcrfs_5, "mcrfs 0,5")     T(t_mcrfs_7, "mcrfs 1,7")

/* Qué valores recibe cada instrucción */
enum {
    V_D2,       /* frA x frB de los valores dobles (pares completos) */
    V_S2,       /* ídem con valores de simple */
    V_D3,       /* frA, frC, frB dobles (muestra) */
    V_S3,       /* ídem simples, más los empates conocidos de fmadds */
    V_D1,       /* frB doble, en los 4 modos de redondeo */
    V_CMP,      /* frA x frB dobles, sin RN */
    V_SEL,      /* frA doble x algunos frC/frB */
    V_LOAD32, V_LOAD64, V_STORE32, V_STORE64,
    V_FPSCR,    /* instrucciones de FPSCR: varios FPSCR de entrada y frB */
};

/* Banderas de la fila */
#define F_KIND_MASK   7u        /* 0 registro, 1 lfs, 2 lfd, 3 stfs/stfiwx, 4 stfd */
#define F_LOW_WORD    0x10u     /* comparar solo la palabra baja de f1 (fctiw, mffs) */
#define F_FULL        0x20u     /* comparar FPSCR y CR enteros (FX, OX, XX...) */
#define F_NO_FPRF     0x40u     /* FPRF sin definir */
#define F_NO_C        0x80u     /* sin FPRF[C] */
#define F_NO_FX       0x100u    /* sin FX */
#define F_SDENORM     0x200u    /* resultado subnormal de simple: QEMU dice "normal" */
/* Solo del generador (no van al archivo) */
#define G_SINGLE      0x10000u  /* instrucción de precisión simple */
#define G_RN01        0x20000u  /* solo RN = 0 y 1 */
#define G_MASK        0xFFFF0000u

typedef struct test {
    const char *name;
    test_fn     fn;
    int         values;
    uint32_t    flags;
    uint32_t    sample;         /* V_D3/V_S3: cuántas ternas */
} test_t;

#define S G_SINGLE
static const test_t tests[] = {
    { "fadd", t_fadd, V_D2, 0, 0 },     { "fsub", t_fsub, V_D2, 0, 0 },
    { "fmul", t_fmul, V_D2, 0, 0 },     { "fdiv", t_fdiv, V_D2, 0, 0 },
    { "fadds", t_fadds, V_S2, S, 0 },   { "fsubs", t_fsubs, V_S2, S, 0 },
    { "fmuls", t_fmuls, V_S2, S, 0 },   { "fdivs", t_fdivs, V_S2, S, 0 },
    { "fmadd", t_fmadd, V_D3, 0, 700 }, { "fmsub", t_fmsub, V_D3, 0, 700 },
    { "fnmadd", t_fnmadd, V_D3, G_RN01, 700 }, { "fnmsub", t_fnmsub, V_D3, G_RN01, 700 },
    { "fmadds", t_fmadds, V_S3, S, 700 }, { "fmsubs", t_fmsubs, V_S3, S, 700 },
    { "fnmadds", t_fnmadds, V_S3, S | G_RN01, 700 }, { "fnmsubs", t_fnmsubs, V_S3, S | G_RN01, 700 },
    { "fsel", t_fsel, V_SEL, 0, 0 },
    { "frsp", t_frsp, V_D1, S, 0 },     { "fctiw", t_fctiw, V_D1, F_LOW_WORD | F_NO_FPRF, 0 },
    { "fctiwz", t_fctiwz, V_D1, F_LOW_WORD | F_NO_FPRF, 0 },
    { "fneg", t_fneg, V_D1, 0, 0 },     { "fabs", t_fabs, V_D1, 0, 0 },
    { "fnabs", t_fnabs, V_D1, 0, 0 },   { "fmr", t_fmr, V_D1, 0, 0 },
    { "fadd.", t_fadd_rc, V_D3, 0, 300 }, { "fmuls.", t_fmuls_rc, V_S3, S, 300 },
    { "fmadd.", t_fmadd_rc, V_D3, 0, 300 }, { "frsp.", t_frsp_rc, V_D1, S, 0 },
    { "fctiw.", t_fctiw_rc, V_D1, F_LOW_WORD | F_NO_FPRF, 0 }, { "fneg.", t_fneg_rc, V_D1, 0, 0 },
    { "fcmpu", t_fcmpu, V_CMP, 0, 0 },  { "fcmpo", t_fcmpo, V_CMP, F_NO_C, 0 },
    { "fcmpu cr0", t_fcmpu0, V_CMP, 0, 0 },
    { "lfs", t_lfs, V_LOAD32, 1, 0 },   { "lfd", t_lfd, V_LOAD64, 2, 0 },
    { "stfs", t_stfs, V_STORE32, 3, 0 }, { "stfd", t_stfd, V_STORE64, 4, 0 },
    { "stfiwx", t_stfiwx, V_STORE32, 3, 0 },
#define FS F_FULL
    { "mffs", t_mffs, V_FPSCR, FS | F_LOW_WORD, 0 }, { "mffs.", t_mffs_rc, V_FPSCR, FS | F_LOW_WORD, 0 },
    { "mtfsf 0xFF", t_mtfsf_ff, V_FPSCR, FS, 0 }, { "mtfsf 0x01", t_mtfsf_01, V_FPSCR, FS, 0 },
    { "mtfsf 0x80", t_mtfsf_80, V_FPSCR, FS, 0 }, { "mtfsf. 0x7E", t_mtfsf_7e, V_FPSCR, FS, 0 },
    { "mtfsfi 0", t_mtfsfi_0, V_FPSCR, FS, 0 },  { "mtfsfi 1", t_mtfsfi_1, V_FPSCR, FS, 0 },
    { "mtfsfi 6", t_mtfsfi_6, V_FPSCR, FS, 0 },  { "mtfsfi. 7", t_mtfsfi_7, V_FPSCR, FS, 0 },
    { "mtfsb0 0", t_mtfsb0_0, V_FPSCR, FS, 0 },  { "mtfsb0 7", t_mtfsb0_7, V_FPSCR, FS, 0 },
    { "mtfsb0. 30", t_mtfsb0_30, V_FPSCR, FS, 0 }, { "mtfsb1 3", t_mtfsb1_3, V_FPSCR, FS | F_NO_FX, 0 },
    { "mtfsb1 7", t_mtfsb1_7, V_FPSCR, FS | F_NO_FX, 0 },  { "mtfsb1 21", t_mtfsb1_21, V_FPSCR, FS | F_NO_FX, 0 },
    { "mtfsb1. 31", t_mtfsb1_31, V_FPSCR, FS | F_NO_FX, 0 }, { "mtfsb1 1", t_mtfsb1_1, V_FPSCR, FS | F_NO_FX, 0 },
    { "mcrfs 2,0", t_mcrfs_0, V_FPSCR, FS, 0 },  { "mcrfs 4,1", t_mcrfs_1, V_FPSCR, FS, 0 },
    { "mcrfs 7,3", t_mcrfs_3, V_FPSCR, FS, 0 },  { "mcrfs 0,5", t_mcrfs_5, V_FPSCR, FS, 0 },
    { "mcrfs 1,7", t_mcrfs_7, V_FPSCR, FS, 0 },
};
#undef S
#undef FS

/* Dobles: especiales, bordes de simple y de enteros, empates de redondeo.
 * Parte de la lista viene de Source/UnitTests/Core/PowerPC/TestValues.h de
 * Dolphin (GPL-2.0-or-later). */
static const uint64_t dvals[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
    0x0010000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FEFFFFFFFFFFFFFull,
    0xFFEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF0000000000001ull,
    0xFFF7FFFFFFFFFFFFull, 0x7FF8000000000000ull, 0xFFFF000000000123ull, 0x3800000000000000ull,
    0xB810000000000000ull, 0x36A0000000000000ull, 0x3FF8000000000000ull, 0x408F400000000000ull,
    0xC008000000000000ull, 0x3FB999999999999Aull, 0x3FD5555555555555ull, 0x400921FB54442D18ull,
    0x41DFFFFFFFC00000ull, 0x41E0000000000000ull, 0xC1E0000000000000ull, 0xC1E0000000200000ull,
    0x4330000000000001ull, 0x3FE0000000000000ull, 0x4004000000000000ull, 0xC00C000000000000ull,
    0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull, 0x3FF0000010000000ull, 0xBFF0000030000000ull,
    0x3FF0000010000001ull, 0x3E80000000000001ull,
};

/* Valores de simple (como bits de float) */
static const uint32_t svals[] = {
    0x00000000, 0x80000000, 0x00000001, 0x00001000, 0x807FFFFF, 0x00800000, 0x3F800000,
    0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7F800001, 0x7FC00000,
    0xFFC00123, 0x3FC00000, 0x447A0000, 0xC0400000, 0x3DCCCCCD, 0x3EAAAAAB, 0x40490FDB,
    0x4B7FFFFF, 0x4B800000, 0x3F000000, 0xBF400000, 0x3F800001, 0x00000003, 0x7E800000,
    0x01000000, 0x33800000,
};

/* fmadds con empates que un FMA doble + redondeo a simple resuelve mal (el
 * primero es el de Mario Strikers Charged citado en Dolphin): a, c, b. */
static const uint32_t madds_ties[][3] = {
    { 0x42480000, 0xBC88CC38, 0x1B1C72A0 },
    { 0xC2480000, 0xBC88CC38, 0x9B1C72A0 },
    { 0x42480000, 0x3C88CC38, 0x9B1C72A0 },
};

/* FPSCR de entrada para las instrucciones de FPSCR. Sin bits de habilitación
 * (VE, OE, UE, ZE, XE): con FEX, QEMU en modo usuario manda SIGFPE. FEX y las
 * trampas se prueban a mano en test_cpu_fpu.c. */
static const uint32_t fpscrs[] = {
    0x00000000, 0x00000003, 0x82000000, 0x81000000, 0xA1080000, 0x0001F000, 0x9E000000,
    0x00000004, 0x00000002, 0x00004001,
};

/* frB de mtfsf (palabra baja = imagen de FPSCR), también sin habilitaciones */
static const uint32_t mtfsf_vals[] = {
    0x00000000, 0xFFFFF707, 0x00000003, 0x80000000, 0x01000000, 0x60000000,
    0x02000000, 0x0001F000, 0x00000004, 0x9A061204,
};

static uint32_t lcg = 0x2545F491u;
static uint32_t next(uint32_t n)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (lcg >> 8) % n;
}

static uint64_t s2d(uint32_t s)    /* float -> double (conversión de lfs) */
{
    float f;
    double d;
    uint64_t u;
    memcpy(&f, &s, 4);
    if ((s & 0x7F800000u) == 0x7F800000u && (s & 0x007FFFFFu)) {
        /* NaN: a mano, para no silenciar una SNaN */
        u = (uint64_t)(s & 0x80000000u) << 32 | 0x7FF0000000000000ull | (uint64_t)(s & 0x007FFFFFu) << 29;
        return u;
    }
    d = f;
    memcpy(&u, &d, 8);
    return u;
}

static FILE *out;
static uint32_t count;

static void put32(uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, out);
}

static void set64(uint32_t *io, int at, uint64_t v)
{
    io[at] = (uint32_t)(v >> 32);
    io[at + 1] = (uint32_t)v;
}

static uint64_t get64(const uint32_t *io, int at)
{
    return (uint64_t)io[at] << 32 | io[at + 1];
}

/* Una fila: 16 palabras.
 *   0 instrucción   1 banderas    2 FPSCR antes   3 CR antes
 *   4-5 frA   6-7 frB   8-9 frC   10-11 f1 antes (o memoria de lfs/lfd)
 *   12-13 f1 después (o memoria después de stfs/stfd)   14 FPSCR   15 CR */
static void run(const test_t *t, uint64_t fa, uint64_t fb, uint64_t fc, uint32_t fpscr)
{
    static uint32_t io[IO_WORDS] __attribute__((aligned(8)));
    const uint32_t kind = t->flags & F_KIND_MASK;
    const uint64_t fd_in = 0x5A5A5A5A12345678ull ^ ((uint64_t)count * 0x9E3779B97F4A7C15ull);
    uint64_t mem_in = 0xDEADBEEFDEADBEEFull;

    if (kind == 1)
        mem_in = fa;                        /* lfs: palabra alta; lfd: las dos */
    else if (kind == 2)
        mem_in = fa;

    memset(io, 0, sizeof io);
    set64(io, IO_FA, fa);
    set64(io, IO_FB, fb);
    set64(io, IO_FC, fc);
    set64(io, IO_FD, fd_in);
    io[IO_FPSCR + 1] = fpscr;
    io[IO_CR] = 0x13579BDFu;
    set64(io, IO_MEM, mem_in);
    t->fn(io);

    const uint64_t fd_out = (kind >= 3) ? get64(io, IO_MEM) : get64(io, IO_FD_OUT);
    uint32_t flags = t->flags & ~G_MASK;
    const uint64_t mag = fd_out & 0x7FFFFFFFFFFFFFFFull;
    if ((t->flags & G_SINGLE) && mag >= 0x36A0000000000000ull && mag < 0x3810000000000000ull)
        flags |= F_SDENORM;
    put32(io[IO_INST]);
    put32(flags);
    put32(io[IO_FPSCR + 1]);                /* el que quedó tras mtfsf */
    put32(0x13579BDFu);
    put32((uint32_t)(fa >> 32));
    put32((uint32_t)fa);
    put32((uint32_t)(fb >> 32));
    put32((uint32_t)fb);
    put32((uint32_t)(fc >> 32));
    put32((uint32_t)fc);
    put32((uint32_t)((kind == 1 || kind == 2 ? mem_in : fd_in) >> 32));
    put32((uint32_t)(kind == 1 || kind == 2 ? mem_in : fd_in));
    put32((uint32_t)(fd_out >> 32));
    put32((uint32_t)fd_out);
    put32(io[IO_FPSCR_OUT + 1]);
    put32(io[IO_CR_OUT]);
    count++;
}

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* stfs de un doble con exponente < 874 (y no cero): sin definir */
static int stfs_undefined(const test_t *t, uint64_t v)
{
    const uint32_t exp = (uint32_t)(v >> 52) & 0x7FF;
    return t->fn == t_stfs && exp < 874 && (v & 0x7FFFFFFFFFFFFFFFull) != 0;
}

static void gen(const test_t *t)
{
    const uint32_t nd = N(dvals), ns = N(svals);
    const uint32_t nrn = (t->flags & G_RN01) ? 2 : 4;
    uint32_t k = 0;

    switch (t->values) {
    case V_D2:
        for (uint32_t a = 0; a < nd; a++)
            for (uint32_t b = 0; b < nd; b++, k++)
                run(t, dvals[a], dvals[b], dvals[b], k % 4);
        break;
    case V_S2:
        for (uint32_t a = 0; a < ns; a++)
            for (uint32_t b = 0; b < ns; b++, k++)
                run(t, s2d(svals[a]), s2d(svals[b]), s2d(svals[b]), k % 4);
        break;
    case V_D3:
        for (k = 0; k < t->sample; k++)
            run(t, dvals[next(nd)], dvals[next(nd)], dvals[next(nd)], k % nrn);
        break;
    case V_S3:
        for (k = 0; k < t->sample; k++)
            run(t, s2d(svals[next(ns)]), s2d(svals[next(ns)]), s2d(svals[next(ns)]), k % nrn);
        for (uint32_t j = 0; j < N(madds_ties); j++)
            for (uint32_t rn = 0; rn < nrn; rn++)
                run(t, s2d(madds_ties[j][0]), s2d(madds_ties[j][2]), s2d(madds_ties[j][1]), rn);
        break;
    case V_D1:
        for (uint32_t b = 0; b < nd; b++)
            for (uint32_t rn = 0; rn < 4; rn++)
                run(t, 0, dvals[b], 0, rn);
        for (uint32_t s = 0; s < ns; s++)
            run(t, 0, s2d(svals[s]) + 0x1000u, 0, s % 4);   /* entre dos simples */
        break;
    case V_CMP:
        for (uint32_t a = 0; a < nd; a++)
            for (uint32_t b = 0; b < nd; b += 2)
                run(t, dvals[a], dvals[b], 0, 0);
        break;
    case V_SEL:
        for (uint32_t a = 0; a < nd; a++)
            for (uint32_t b = 0; b < 4; b++)
                run(t, dvals[a], dvals[11 + b], dvals[5 + b], 0);
        break;
    case V_LOAD32:
        for (uint32_t s = 0; s < ns; s++)
            run(t, (uint64_t)svals[s] << 32 | 0xCAFEF00Du, 0, 0, 0);
        break;
    case V_LOAD64:
        for (uint32_t d = 0; d < nd; d++)
            run(t, dvals[d], 0, 0, 0);
        break;
    case V_STORE32:
    case V_STORE64:
        for (uint32_t d = 0; d < nd; d++)
            if (!stfs_undefined(t, dvals[d]))
                run(t, dvals[d], 0, 0, d % 4);
        for (uint32_t s = 0; s < ns; s++)
            run(t, s2d(svals[s]), 0, 0, 0);
        /* stfs con exponentes de 874 a 896: se desnormaliza a mano */
        for (uint32_t e = 874; e <= 898; e++)
            run(t, (uint64_t)e << 52 | 0x000ABCDEF1234567ull, 0, 0, 0);
        break;
    case V_FPSCR:
        for (uint32_t f = 0; f < N(fpscrs); f++)
            for (uint32_t v = 0; v < N(mtfsf_vals); v += 3)
                run(t, 0, 0xFFF8000000000000ull | mtfsf_vals[v], 0, fpscrs[f]);
        break;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "oracle_fpu.bin";
    out = fopen(path, "wb");
    if (!out) {
        perror(path);
        return 1;
    }
    fwrite("GCOF", 1, 4, out);
    put32(1);
    put32(0);                               /* cantidad, se completa al final */
    for (size_t t = 0; t < N(tests); t++)
        gen(&tests[t]);
    fseek(out, 8, SEEK_SET);
    put32(count);
    fclose(out);
    fprintf(stderr, "%s: %u casos de %u instrucciones\n", path, count, (unsigned)N(tests));
    return 0;
}
