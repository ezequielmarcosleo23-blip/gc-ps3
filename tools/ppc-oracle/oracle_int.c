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
 * Oráculo de enteros: ejecuta instrucciones de verdad en un PowerPC 750CXe
 * emulado por QEMU (la base de Gekko) y guarda entradas y resultados para las
 * pruebas del intérprete (tests/unit/test_cpu_int.c).
 *
 *     make oracle      (necesita gcc-powerpc-linux-gnu y qemu-user)
 *
 * Cada caso usa r3 como destino, r4 y r5 como fuentes. Formato de salida
 * (big-endian, 10 palabras por caso):
 *   inst, a, b, d_in, xer_in, cr_in, d_out, a_out, xer_out, cr_out
 * precedido por "GCOR", versión y número de casos.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void (*test_fn)(uint32_t *io);

#define T(fname, text)                                                          \
    static void fname(uint32_t *io)                                             \
    {                                                                           \
        __asm__ volatile("lwz 4,4(%0)\n\t"                                      \
                         "lwz 5,8(%0)\n\t"                                      \
                         "lwz 3,12(%0)\n\t"                                     \
                         "lwz 6,16(%0)\n\t"                                     \
                         "mtxer 6\n\t"                                          \
                         "lwz 6,20(%0)\n\t"                                     \
                         "mtcr 6\n\t"                                           \
                         "0: " text "\n\t"                                      \
                         "stw 3,24(%0)\n\t"                                     \
                         "stw 4,28(%0)\n\t"                                     \
                         "mfxer 6\n\t"                                          \
                         "stw 6,32(%0)\n\t"                                     \
                         "mfcr 6\n\t"                                           \
                         "stw 6,36(%0)\n\t"                                     \
                         "lis 6,0b@ha\n\t"                                      \
                         "addi 6,6,0b@l\n\t"                                    \
                         "lwz 6,0(6)\n\t"                                       \
                         "stw 6,0(%0)\n\t"                                      \
                         :                                                      \
                         : "b"(io)                                              \
                         : "r3", "r4", "r5", "r6", "xer", "cr0", "cr1", "cr2",  \
                           "cr3", "cr4", "cr5", "cr6", "cr7", "memory");        \
    }

/* Formas XO: normal y con OE + Rc (cubre OV, SO, CA y CR0). */
#define XO2(op) T(t_##op, #op " 3,4,5") T(t_##op##_oe, #op "o. 3,4,5")
#define XO1(op) T(t_##op, #op " 3,4")   T(t_##op##_oe, #op "o. 3,4")
#define LG2(op) T(t_##op, #op " 3,4,5") T(t_##op##_rc, #op ". 3,4,5")
#define LG1(op) T(t_##op, #op " 3,4")   T(t_##op##_rc, #op ". 3,4")

XO2(add) XO2(addc) XO2(adde) XO2(subf) XO2(subfc) XO2(subfe) XO2(mullw) XO2(divw) XO2(divwu)
XO1(addme) XO1(addze) XO1(subfme) XO1(subfze) XO1(neg)
LG2(mulhw) LG2(mulhwu)
LG2(and) LG2(andc) LG2(or) LG2(orc) LG2(xor) LG2(nand) LG2(nor) LG2(eqv)
LG2(slw) LG2(srw) LG2(sraw)
LG1(cntlzw) LG1(extsb) LG1(extsh)
T(t_cmpw, "cmpw 3,4,5")  T(t_cmplw, "cmplw 3,4,5")
T(t_cmpw0, "cmpw 4,5")   T(t_cmplw7, "cmplw 7,4,5")

/* Inmediatos */
#define IMM(name, op, imm) T(t_##name, #op " 3,4," #imm)
IMM(addi_0, addi, 0)        IMM(addi_m1, addi, -1)      IMM(addi_max, addi, 32767)  IMM(addi_min, addi, -32768)
IMM(addis_1, addis, 1)      IMM(addis_m1, addis, -1)    IMM(addis_min, addis, -32768)
IMM(addic_1, addic, 1)      IMM(addic_m1, addic, -1)    IMM(addic_min, addic, -32768)
IMM(addicr_1, addic., 1)    IMM(addicr_m1, addic., -1)  IMM(addicr_max, addic., 32767)
IMM(subfic_0, subfic, 0)    IMM(subfic_1, subfic, 1)    IMM(subfic_m1, subfic, -1)  IMM(subfic_min, subfic, -32768)
IMM(mulli_3, mulli, 3)      IMM(mulli_m1, mulli, -1)    IMM(mulli_max, mulli, 32767) IMM(mulli_min, mulli, -32768)
IMM(ori_0, ori, 0)          IMM(ori_ffff, ori, 0xFFFF)  IMM(oris_8000, oris, 0x8000)
IMM(xori_5555, xori, 0x5555) IMM(xoris_ffff, xoris, 0xFFFF)
IMM(andi_ff, andi., 0xFF)   IMM(andi_8000, andi., 0x8000) IMM(andis_8000, andis., 0x8000) IMM(andis_ffff, andis., 0xFFFF)
T(t_cmpwi_0, "cmpwi 2,4,0")       T(t_cmpwi_m1, "cmpwi 2,4,-1")      T(t_cmpwi_max, "cmpwi 2,4,32767")
T(t_cmplwi_0, "cmplwi 5,4,0")     T(t_cmplwi_ffff, "cmplwi 5,4,0xFFFF")

/* Desplazamiento algebraico inmediato y rotaciones */
T(t_srawi_0, "srawi 3,4,0")   T(t_srawi_1, "srawi. 3,4,1")   T(t_srawi_4, "srawi 3,4,4")   T(t_srawi_31, "srawi. 3,4,31")
T(t_rlwinm_a, "rlwinm 3,4,0,0,31")   T(t_rlwinm_b, "rlwinm. 3,4,8,24,31")  T(t_rlwinm_c, "rlwinm 3,4,31,1,31")
T(t_rlwinm_d, "rlwinm 3,4,16,0,15")  T(t_rlwinm_e, "rlwinm. 3,4,4,28,3")   T(t_rlwinm_f, "rlwinm 3,4,0,31,0")
T(t_rlwinm_g, "rlwinm 3,4,27,5,31")  T(t_rlwinm_h, "rlwinm. 3,4,1,0,30")
T(t_rlwimi_a, "rlwimi 3,4,0,16,31")  T(t_rlwimi_b, "rlwimi. 3,4,8,16,23")  T(t_rlwimi_c, "rlwimi 3,4,24,8,4")
T(t_rlwimi_d, "rlwimi 3,4,31,0,0")
T(t_rlwnm_a, "rlwnm 3,4,5,0,31")     T(t_rlwnm_b, "rlwnm. 3,4,5,16,31")    T(t_rlwnm_c, "rlwnm 3,4,5,28,3")

typedef struct test {
    const char *name;
    test_fn     fn;
    int         shift;      /* b se usa como cantidad de desplazamiento */
} test_t;

#define E2(op) { #op, t_##op, 0 }, { #op "o.", t_##op##_oe, 0 }
#define L2(op) { #op, t_##op, 0 }, { #op ".", t_##op##_rc, 0 }
#define S2(op) { #op, t_##op, 1 }, { #op ".", t_##op##_rc, 1 }
#define I(name) { #name, t_##name, 0 }
#define R(name) { #name, t_##name, 1 }

static const test_t tests[] = {
    E2(add), E2(addc), E2(adde), E2(subf), E2(subfc), E2(subfe), E2(mullw), E2(divw), E2(divwu),
    E2(addme), E2(addze), E2(subfme), E2(subfze), E2(neg),
    L2(mulhw), L2(mulhwu),
    L2(and), L2(andc), L2(or), L2(orc), L2(xor), L2(nand), L2(nor), L2(eqv),
    S2(slw), S2(srw), S2(sraw),
    L2(cntlzw), L2(extsb), L2(extsh),
    I(cmpw), I(cmplw), I(cmpw0), I(cmplw7),
    I(addi_0), I(addi_m1), I(addi_max), I(addi_min), I(addis_1), I(addis_m1), I(addis_min),
    I(addic_1), I(addic_m1), I(addic_min), I(addicr_1), I(addicr_m1), I(addicr_max),
    I(subfic_0), I(subfic_1), I(subfic_m1), I(subfic_min),
    I(mulli_3), I(mulli_m1), I(mulli_max), I(mulli_min),
    I(ori_0), I(ori_ffff), I(oris_8000), I(xori_5555), I(xoris_ffff),
    I(andi_ff), I(andi_8000), I(andis_8000), I(andis_ffff),
    I(cmpwi_0), I(cmpwi_m1), I(cmpwi_max), I(cmplwi_0), I(cmplwi_ffff),
    I(srawi_0), I(srawi_1), I(srawi_4), I(srawi_31),
    I(rlwinm_a), I(rlwinm_b), I(rlwinm_c), I(rlwinm_d), I(rlwinm_e), I(rlwinm_f), I(rlwinm_g), I(rlwinm_h),
    I(rlwimi_a), I(rlwimi_b), I(rlwimi_c), I(rlwimi_d),
    R(rlwnm_a), R(rlwnm_b), R(rlwnm_c),
};

static const uint32_t values[] = {
    0x00000000, 0x00000001, 0x00007FFF, 0x00008000, 0x0000FFFF,
    0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xFEDCBA98,
};

static const uint32_t shifts[] = { 0, 1, 15, 31, 32, 33, 63, 0xFFFFFFE1u };

static const uint32_t xers[] = { 0x00000000, 0x20000000, 0x80000000, 0xA0000000 };

static void put32(FILE *f, uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

/* División con resultado sin definir: el intérprete sigue a Dolphin, no al oráculo. */
static int undefined_case(const char *name, uint32_t a, uint32_t b)
{
    if (strncmp(name, "divw", 4) != 0)
        return 0;
    if (b == 0)
        return 1;
    return name[4] != 'u' && a == 0x80000000u && b == 0xFFFFFFFFu;
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "oracle_int.bin";
    FILE *f = fopen(out, "wb");
    if (!f) {
        perror(out);
        return 1;
    }
    const size_t ntests = sizeof tests / sizeof tests[0];
    const size_t nval = sizeof values / sizeof values[0];
    uint32_t count = 0;

    fwrite("GCOR", 1, 4, f);
    put32(f, 1);
    put32(f, 0);                            /* número de casos, se completa al final */
    for (size_t t = 0; t < ntests; t++) {
        const size_t nb = tests[t].shift ? sizeof shifts / sizeof shifts[0] : nval;
        size_t k = 0;
        for (size_t ia = 0; ia < nval; ia++)
            for (size_t ib = 0; ib < nb; ib++, k++) {
                uint32_t io[10];
                const uint32_t a = values[ia];
                const uint32_t b = tests[t].shift ? shifts[ib] : values[ib];
                if (undefined_case(tests[t].name, a, b))
                    continue;
                memset(io, 0, sizeof io);
                io[1] = a;
                io[2] = b;
                io[3] = 0xDEADBEEFu ^ (uint32_t)(k * 0x9E3779B9u);
                io[4] = xers[k % 4];
                io[5] = 0x13579BDFu;
                tests[t].fn(io);
                put32(f, io[0]);
                for (int w = 1; w < 6; w++)
                    put32(f, io[w]);
                put32(f, io[6]);
                put32(f, io[7]);
                put32(f, io[8]);
                put32(f, io[9]);
                count++;
            }
    }
    fseek(f, 8, SEEK_SET);
    put32(f, count);
    fclose(f);
    fprintf(stderr, "%s: %u casos de %u instrucciones\n", out, count, (unsigned)ntests);
    return 0;
}
