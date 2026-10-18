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
 * Patrón de los archivos de prueba de lectura (prueba "iso" de la Fase 0).
 *
 * Cada palabra de 32 bits, en big-endian, vale su propia posición en bytes:
 * la palabra que empieza en el byte 0x12345678 es 12 34 56 78. Así cualquier
 * trozo leído se verifica sin copia de referencia y una lectura del lugar
 * equivocado se detecta aunque los datos "parezcan" bien. Pasados 4 GB el
 * valor da la vuelta (solo cuentan los 32 bits bajos).
 *
 * tools/mkiso.py genera los archivos con el mismo patrón.
 */
#ifndef GCPS3_CORE_PATTERN_H
#define GCPS3_CORE_PATTERN_H

#include <stddef.h>
#include <stdint.h>

/* Tamaño de un disco de GameCube (DVD de 8 cm): 712 880 sectores de 2048 bytes. */
#define PATTERN_GC_DISC_SIZE 1459978240ull

/* Llena buf con el patrón de [offset, offset + len). offset y len múltiplos de 4. */
void pattern_fill(void *buf, uint64_t offset, size_t len);

/* Comprueba buf contra el patrón de [offset, offset + len). Devuelve -1 si
 * coincide, o la posición (en bytes, dentro de buf) de la primera palabra
 * distinta. offset y len múltiplos de 4. */
long pattern_check(const void *buf, uint64_t offset, size_t len);

#endif
