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
 * Lo mínimo para los DOLs de prueba, sin libc (D-034). Las funciones de salida
 * tienen nombres que el emulador reconoce en el .map (src/core/hle_os.c) y
 * cuerpo vacío: en el hardware no hacen nada; en el emulador (o en Dolphin) su
 * texto va al log.
 *
 * Con GCPS3_HOST, el mismo programa se compila para la PC con la libc normal:
 * su salida es la que se espera del DOL (tests/dol/esperado/).
 */
#ifndef GCPS3_TESTDOL_RT_H
#define GCPS3_TESTDOL_RT_H

#include <stdarg.h>
#include <stdint.h>

#ifdef GCPS3_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#else
typedef __SIZE_TYPE__ size_t;
int  vprintf(const char *fmt, va_list ap);
int  puts(const char *s);
void exit(int code) __attribute__((noreturn));
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
#endif

void OSReport(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
