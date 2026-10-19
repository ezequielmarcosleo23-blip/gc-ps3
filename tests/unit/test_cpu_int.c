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
 * Instrucciones de enteros contra el oráculo: tests/unit/data/oracle_int.bin,
 * generado ejecutando cada instrucción en un PowerPC 750CXe emulado
 * (tools/ppc-oracle). Cada caso pone r4 = a, r5 = b, r3 = d, XER y CR, ejecuta
 * la instrucción y compara r3, r4, XER y CR.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/mem.h"
#include "cpu/gekko.h"
#include "test.h"

#define ORACLE_INT_PATH "tests/unit/data/oracle_int.bin"
#define CODE_ADDR       0x80001000u
#define MAX_REPORTS     12

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Lee el archivo entero; NULL si falta o no tiene el formato. */
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
    if (memcmp(buf, "GCOR", 4) != 0 || rd32(buf + 4) != 1 || 12 + (long)*count * 40 != size) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void test_oracle_int(void)
{
    static gc_mem_t mem;
    static gekko_t cpu;
    uint32_t count = 0, bad = 0;
    uint8_t *data = load_oracle(ORACLE_INT_PATH, &count);
    CHECK(data != NULL);

    gc_log_set_sink(quiet);
    CHECK(gc_mem_init(&mem) == 0);
    gekko_reset(&cpu, &mem);

    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *row = data + 12 + (size_t)k * 40;
        const uint32_t inst = rd32(row), a = rd32(row + 4), b = rd32(row + 8);
        const uint32_t d = rd32(row + 12), xer = rd32(row + 16), cr = rd32(row + 20);
        const uint32_t d_out = rd32(row + 24), a_out = rd32(row + 28);
        const uint32_t xer_out = rd32(row + 32), cr_out = rd32(row + 36);

        gc_write32(&mem, CODE_ADDR, inst);
        cpu.pc = CODE_ADDR;
        cpu.msr = 0;
        cpu.gpr[3] = d;
        cpu.gpr[4] = a;
        cpu.gpr[5] = b;
        cpu.xer = xer;
        cpu.cr = cr;
        gekko_step(&cpu);

        if (cpu.gpr[3] != d_out || cpu.gpr[4] != a_out || cpu.xer != xer_out || cpu.cr != cr_out ||
            cpu.pc != CODE_ADDR + 4) {
            if (bad++ < MAX_REPORTS)
                fprintf(stderr,
                        "  oraculo: inst %08x (op %u xo %u) a=%08x b=%08x xer=%08x -> "
                        "r3 %08x/%08x r4 %08x/%08x xer %08x/%08x cr %08x/%08x (obtenido/esperado)\n",
                        (unsigned)inst, (unsigned)(inst >> 26), (unsigned)((inst >> 1) & 0x3FF),
                        (unsigned)a, (unsigned)b, (unsigned)xer, (unsigned)cpu.gpr[3], (unsigned)d_out,
                        (unsigned)cpu.gpr[4], (unsigned)a_out, (unsigned)cpu.xer, (unsigned)xer_out,
                        (unsigned)cpu.cr, (unsigned)cr_out);
        }
    }
    gc_mem_free(&mem);
    gc_log_set_sink(NULL);
    free(data);
    if (bad)
        fprintf(stderr, "  oraculo: %u de %u casos distintos\n", (unsigned)bad, (unsigned)count);
    CHECK(bad == 0);
    CHECK(count > 10000);
}

const test_case_t cpu_int_tests[] = {
    { "gekko: enteros contra el oraculo (750CXe en QEMU)", test_oracle_int },
    { NULL, NULL },
};
