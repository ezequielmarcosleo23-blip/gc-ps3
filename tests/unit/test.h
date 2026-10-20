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

/* Marco mínimo de pruebas unitarias: sin dependencias, pensado para ASan/UBSan. */
#ifndef GCPS3_TESTS_TEST_H
#define GCPS3_TESTS_TEST_H

#include <stdint.h>
#include <stdio.h>

extern int test_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            test_failures++;                                                   \
            return;                                                            \
        }                                                                      \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                         \
    do {                                                                       \
        uint64_t a_ = (uint64_t)(actual), e_ = (uint64_t)(expected);           \
        if (a_ != e_) {                                                        \
            fprintf(stderr, "  FALLO %s:%d: %s = 0x%llx, esperado 0x%llx\n",   \
                    __FILE__, __LINE__, #actual,                               \
                    (unsigned long long)a_, (unsigned long long)e_);           \
            test_failures++;                                                   \
            return;                                                            \
        }                                                                      \
    } while (0)

typedef struct test_case {
    const char *name;
    void (*fn)(void);
} test_case_t;

/* Cada archivo de pruebas expone su lista terminada en {NULL, NULL}. */
extern const test_case_t endian_tests[];
extern const test_case_t backend_tests[];
extern const test_case_t logline_tests[];
extern const test_case_t pattern_tests[];
extern const test_case_t mem_tests[];
extern const test_case_t mmio_tests[];
extern const test_case_t cpu_int_tests[];
extern const test_case_t cpu_tests[];
extern const test_case_t cpu_fpu_tests[];
extern const test_case_t cpu_ps_tests[];
extern const test_case_t dol_tests[];
extern const test_case_t sched_tests[];
extern const test_case_t system_tests[];
extern const test_case_t hw_tests[];
extern const test_case_t hw_io_tests[];
extern const test_case_t xfb_tests[];
extern const test_case_t hle_tests[];
extern const test_case_t program_tests[];

#endif
