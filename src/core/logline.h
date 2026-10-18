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
 * Formato de una línea del log por UDP (un datagrama = una línea):
 *
 *     GCPS3 <sesión:8 hex> <secuencia> <t_us> <nivel> <prueba> | <mensaje>
 *     GCPS3 0000beef 7 1500 I hello | hola mundo
 *
 * - sesión: identifica una ejecución; el receptor separa los logs por sesión.
 * - secuencia: empieza en 0 y sube de 1 en 1; un hueco = datagrama perdido.
 * - t_us: microsegundos desde el inicio del log en la consola.
 * - nivel: I (info), W (aviso), E (error), R (resultado final).
 * - prueba: [a-z0-9_-], hasta 31 caracteres.
 * Ver docs/log-udp.md. tools/logrecv.py implementa el mismo formato.
 */
#ifndef GCPS3_CORE_LOGLINE_H
#define GCPS3_CORE_LOGLINE_H

#include <stddef.h>
#include <stdint.h>

#define LOGLINE_MAGIC     "GCPS3"
#define LOGLINE_PORT      18194
#define LOGLINE_MAX       512     /* bytes por datagrama, con el '\0' */
#define LOGLINE_TEST_MAX  31

typedef struct logline {
    uint32_t    session;
    uint32_t    seq;
    uint64_t    t_us;
    char        level;
    char        test[LOGLINE_TEST_MAX + 1];
    const char *msg;               /* apunta dentro de la línea analizada */
} logline_t;

/* 1 si el nombre de prueba es válido ([a-z0-9_-], 1..31 caracteres). */
int logline_valid_test_name(const char *test);

/* 1 si el nivel es uno de I, W, E, R. */
int logline_valid_level(char level);

/*
 * Escribe la línea en buf (terminada en '\0', sin salto de línea).
 * Si el mensaje no cabe, se trunca y la línea sigue siendo válida.
 * Devuelve la longitud escrita, o -1 si los argumentos no son válidos.
 */
int logline_format(char *buf, size_t size, uint32_t session, uint32_t seq,
                   uint64_t t_us, char level, const char *test, const char *msg);

/* Analiza una línea; 0 si es válida (out->msg apunta dentro de line), -1 si no. */
int logline_parse(const char *line, logline_t *out);

#endif
