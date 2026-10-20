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

/* Formateador con el ABI del PowerPC, lector de .map y ganchos HLE. */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "core/vaformat.h"
#include "test.h"

#define FMT   0x80100000u
#define STR   0x80100800u
#define STACK 0x80200000u

static gc_system_t sys;
static char out[512];

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static int setup(void)
{
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    const gc_boot_config_t cfg = { 0, 0 };
    gc_system_hle_boot(&sys, &cfg, 0x80003000u);
    sys.cpu.gpr[1] = STACK;
    return 0;
}

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

static void put_str(uint32_t addr, const char *s)
{
    do
        gc_write8(&sys.mem, addr++, (uint8_t)*s);
    while (*s++);
}

static void set_fpr(int r, double d)
{
    memcpy(&sys.cpu.fpr[r][0], &d, 8);
}

static const char *fmt_regs(const char *fmt)
{
    gc_va_t va;
    put_str(FMT, fmt);
    gc_va_from_regs(&va, &sys.mem, &sys.cpu, 4);
    gc_vaformat(&sys.mem, FMT, &va, out, sizeof out);
    return out;
}

static void test_format_registers(void)
{
    gekko_t *c = &sys.cpu;
    CHECK(setup() == 0);
    put_str(STR, "hola");

    c->gpr[4] = (uint32_t)-5; c->gpr[5] = 7; c->gpr[6] = 0xAB; c->gpr[7] = STR; c->gpr[8] = 'z';
    CHECK(strcmp(fmt_regs("%d %u %x %s %c"), "-5 7 ab hola z") == 0);

    c->gpr[4] = 1; c->gpr[5] = 0x00000001u; c->gpr[6] = 0x00000002u;      /* long long en r5:r6 */
    CHECK(strcmp(fmt_regs("%d %lld"), "1 4294967298") == 0);
    c->gpr[4] = 0xFFFFFFFFu; c->gpr[5] = 0; c->gpr[6] = 3;                 /* r4 se salta: par impar */
    CHECK(strcmp(fmt_regs("%lld"), "3") == 0);

    /* seis enteros en r4-r9, el long long no entra en r10: pila; el último
     * entero también va a la pila (no se vuelve a r10) */
    for (int r = 4; r <= 9; r++)
        c->gpr[r] = (uint32_t)(r - 3);
    c->gpr[10] = 99;
    gc_write64(&sys.mem, STACK + 8, 77);
    gc_write32(&sys.mem, STACK + 16, 8);
    CHECK(strcmp(fmt_regs("%d %d %d %d %d %d %lld %d"), "1 2 3 4 5 6 77 8") == 0);

    /* nueve double: f1-f8 y uno en la pila (alineado a 8) */
    for (int f = 1; f <= 8; f++)
        set_fpr(f, f + 0.5);
    double nine = 9.5;
    uint64_t bits;
    memcpy(&bits, &nine, 8);
    gc_write64(&sys.mem, STACK + 8, bits);
    CHECK(strcmp(fmt_regs("%g %g %g %g %g %g %g %g %g"), "1.5 2.5 3.5 4.5 5.5 6.5 7.5 8.5 9.5") == 0);

    c->gpr[4] = 5; c->gpr[5] = 42; c->gpr[6] = 2; c->gpr[7] = 0; set_fpr(1, 3.14159); set_fpr(2, 2.5);
    CHECK(strcmp(fmt_regs("[%*d] [%.*f] [%s] [%05.1f]"), "[   42] [3.14] [(null)] [002.5]") == 0);
    c->gpr[4] = 70000; c->gpr[5] = 300;
    CHECK(strcmp(fmt_regs("%hd %hhu"), "4464 44") == 0);
    c->gpr[4] = 0x1234;
    CHECK(strcmp(fmt_regs("100%% %y %p %"), "100% %y 0x1234 %") == 0);  /* desconocida y cortada */
    teardown();
}

static void test_format_va_list(void)
{
    const uint32_t va = 0x80300000u, regs = 0x80300100u, over = 0x80300200u;
    gc_va_t v;
    CHECK(setup() == 0);
    /* va_start después de un argumento fijo y de r4: gpr = 2 usados, fpr = 0 */
    gc_write8(&sys.mem, va, 2);
    gc_write8(&sys.mem, va + 1, 0);
    gc_write32(&sys.mem, va + 4, over);
    gc_write32(&sys.mem, va + 8, regs);
    for (uint32_t r = 0; r < 8; r++)
        gc_write32(&sys.mem, regs + 4 * r, 100 + r);                 /* r3..r10 guardados */
    double d = 0.25;
    uint64_t bits;
    memcpy(&bits, &d, 8);
    gc_write64(&sys.mem, regs + 32, bits);                           /* f1 */
    gc_write32(&sys.mem, over, 555);
    put_str(FMT, "%d %d %lld %g %d %d");
    gc_va_from_list(&v, &sys.mem, &sys.cpu, va);
    gc_vaformat(&sys.mem, FMT, &v, out, sizeof out);
    /* r5=102, r6=103, par r7:r8 = 104:105, f1, r9=106, r10=107 */
    CHECK(strcmp(out, "102 103 446676598889 0.25 106 107") == 0);
    put_str(FMT, "%d %d %d %d %d %d %d");
    gc_va_from_list(&v, &sys.mem, &sys.cpu, va);
    gc_vaformat(&sys.mem, FMT, &v, out, sizeof out);
    CHECK(strcmp(out, "102 103 104 105 106 107 555") == 0);           /* el séptimo, de la pila */

    char small[6];
    put_str(FMT, "abcdefghij");
    gc_va_from_regs(&v, &sys.mem, &sys.cpu, 4);
    CHECK(gc_vaformat(&sys.mem, FMT, &v, small, sizeof small) == 5 && strcmp(small, "abcde") == 0);
    teardown();
}

static char lines[4][64];
static int nlines;

static void collect(void *ctx, const char *line)
{
    (void)ctx;
    if (nlines < 4)
        snprintf(lines[nlines], sizeof lines[0], "%s", line);
    nlines++;
}

/* Un programa que llama a OSReport y a exit, con el .map en tres formatos. */
static void test_hooks(void)
{
    static const uint32_t prog[] = {
        0x3C608010u,    /* 0x80003000: lis r3,0x8010        (formato) */
        0x3880002Au,    /*             li r4,42 */
        0x480000F9u,    /*             bl 0x80003100 (OSReport) */
        0x38600007u,    /*             li r3,7 */
        0x480001F1u,    /*             bl 0x80003200 (exit) */
        0x48000000u,    /*             b . */
    };
    static const uint32_t osreport[] = { 0x9421FFF0u, 0x38210010u, 0x4E800020u };
    static const char map[] =
        "  Starting        Virtual\n"
        ".text section layout\n"
        "80003100 00000c 80003100  4 OSReport\n"                    /* 3 columnas + alineación */
        "80003200 000004 80003200 00001200 exit\t(crt0.o)\n"        /* 4 columnas */
        "80003300 noesfuncion\n"
        "\n"
        ".data section layout\n"
        "80003400 000004 80003400 0 puts\n";                         /* datos: no se engancha */
    CHECK(setup() == 0);
    for (uint32_t k = 0; k < 6; k++)
        gc_write32(&sys.mem, 0x80003000u + 4 * k, prog[k]);
    for (uint32_t k = 0; k < 3; k++)
        gc_write32(&sys.mem, 0x80003100u + 4 * k, osreport[k]);
    gc_write32(&sys.mem, 0x80003200u, 0x48000000u);
    put_str(FMT, "valor %d\nsegunda ");

    CHECK(gc_system_load_map(&sys, map, sizeof map - 1) == 2);
    CHECK(gc_read32(&sys.mem, 0x80003100u) == GEKKO_HLE_INST(0));
    CHECK(gc_hle_original(&sys, GEKKO_HLE_INST(0)) == 0x9421FFF0u);
    CHECK(gc_hle_original(&sys, 0x38600007u) == 0x38600007u);
    CHECK(gc_read32(&sys.mem, 0x80003400u) == 0);

    nlines = 0;
    sys.hle.report = collect;
    gc_system_run(&sys, 1000);
    CHECK(sys.cpu.stop && sys.hle.exited && sys.hle.exit_code == 7);
    CHECK(sys.cpu.gpr[1] == STACK);                     /* el stwu original corrió */
    CHECK(sys.cpu.illegal == 0);
    CHECK(nlines == 2 && strcmp(lines[0], "valor 42") == 0 && strcmp(lines[1], "segunda ") == 0);

    /* sin gancho registrado, el opcode 1 es ilegal */
    sys.cpu.hle = NULL;
    sys.cpu.stop = 0;
    sys.cpu.msr = MSR_FP;
    sys.cpu.pc = 0x80003100u;
    gekko_step(&sys.cpu);
    CHECK(sys.cpu.pc == EXC_PROGRAM && sys.cpu.illegal == 1);
    teardown();
}

const test_case_t hle_tests[] = {
    { "hle: printf con argumentos en registros y pila (ABI SVR4)", test_format_registers },
    { "hle: vprintf con va_list en la RAM", test_format_va_list },
    { "hle: .map de Dolphin, ganchos al inicio y exit", test_hooks },
    { NULL, NULL },
};
