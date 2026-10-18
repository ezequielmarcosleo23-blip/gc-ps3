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
 * Base común de las pruebas de la Fase 0 en consola.
 *
 *     static int mi_prueba(const ps3_env_t *env) { ...; return 0; }   // 0 = OK
 *     int main(void) { return phase0_main("mi_prueba", mi_prueba); }
 *
 * phase0_main inicia el log UDP, registra versión y entorno, ejecuta la prueba,
 * escribe "RESULT: OK/FAIL", suena el buzzer en consola (1 pitido OK, 3 FAIL)
 * y devuelve el control al loader (ps3loadx) o al XMB.
 */
#ifndef GCPS3_TESTS_PHASE0_HARNESS_H
#define GCPS3_TESTS_PHASE0_HARNESS_H

#include "platform/ps3/env.h"
#include "platform/ps3/udplog.h"

typedef int (*phase0_test_fn)(const ps3_env_t *env);

int phase0_main(const char *name, phase0_test_fn fn);

/* Atajos de log para las pruebas. */
#define LOGI(...) udplog_printf('I', __VA_ARGS__)
#define LOGW(...) udplog_printf('W', __VA_ARGS__)
#define LOGE(...) udplog_printf('E', __VA_ARGS__)

#endif
