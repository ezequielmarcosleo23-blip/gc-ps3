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
 * Prueba "hello" (etapa B): comprueba que el log UDP llega a la PC.
 * Éxito = en la PC aparecen las 5 líneas numeradas, el entorno y RESULT: OK.
 */
#include <sys/systime.h>

#include "common/harness.h"

static int test_hello(const ps3_env_t *env)
{
    (void)env;
    for (int i = 1; i <= 5; i++) {
        LOGI("hola desde la PS3, linea %d de 5", i);
        sysUsleep(100 * 1000);
    }
    return 0;
}

int main(void)
{
    return phase0_main("hello", test_hello);
}
