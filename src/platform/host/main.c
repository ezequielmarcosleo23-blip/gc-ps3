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
 * Runner en PC: ejecuta el núcleo portable sin consola. En la Fase 0 solo
 * existe el esqueleto; a partir de la Fase 1 cargará una ISO y correrá el
 * intérprete de Gekko con trazas comparables con Dolphin.
 */
#include <stdio.h>
#include <string.h>

#include "core/version.h"

static void usage(const char *argv0)
{
    printf("uso: %s [--version] [--help]\n", argv0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s (runner de PC)\n", GCPS3_NAME, GCPS3_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        fprintf(stderr, "opcion desconocida: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }
    printf("%s %s: el runner todavia no ejecuta nada (Fase 0)\n", GCPS3_NAME, GCPS3_VERSION);
    return 0;
}
