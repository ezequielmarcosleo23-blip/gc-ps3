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
#include <stdio.h>

#include "test.h"

int test_failures = 0;

static const test_case_t *const suites[] = {
    endian_tests,
    backend_tests,
    logline_tests,
    pattern_tests,
    mem_tests,
    mmio_tests,
    cpu_int_tests,
    cpu_tests,
    cpu_fpu_tests,
    cpu_ps_tests,
    dol_tests,
    sched_tests,
    system_tests,
    hw_tests,
    hw_io_tests,
    xfb_tests,
    hle_tests,
    program_tests,
};

int main(void)
{
    int total = 0;

    /* Sin búfer: si ASan corta el proceso (p. ej. por memoria que una prueba
     * fallida no liberó), lo ya impreso no se pierde en un pipe. */
    setvbuf(stdout, NULL, _IONBF, 0);

    for (size_t s = 0; s < sizeof suites / sizeof suites[0]; s++) {
        for (const test_case_t *t = suites[s]; t->name; t++) {
            int before = test_failures;
            t->fn();
            printf("[%s] %s\n", test_failures == before ? " OK " : "FALLO", t->name);
            total++;
        }
    }

    printf("%d pruebas, %d fallos\n", total, test_failures);
    return test_failures ? 1 : 0;
}
