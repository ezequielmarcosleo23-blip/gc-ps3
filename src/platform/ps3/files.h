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
 * Archivos en la PS3 con las llamadas de lv2 (las mismas que usa la prueba
 * iso de la Fase 0, probadas en RPCS3): leer un archivo entero y listar los
 * DOLs de una carpeta.
 */
#ifndef GCPS3_PLATFORM_PS3_FILES_H
#define GCPS3_PLATFORM_PS3_FILES_H

#include <stddef.h>
#include <stdint.h>

#define PS3_PATH_MAX 256
#define PS3_NAME_MAX 64

typedef struct ps3_dol {
    char name[PS3_NAME_MAX];          /* "pantalla.dol" */
    char path[PS3_PATH_MAX];          /* ruta completa */
    int  bundled;                     /* 1: viene con la app (USRDIR/dol) */
} ps3_dol_t;

/* Lee el archivo entero en un búfer nuevo (free). 0, o -1 si no se puede o
 * pasa de max bytes. */
int ps3_read_file(const char *path, uint8_t **data, size_t *len, size_t max);

/* Agrega los .dol de dir a list (desde *count, hasta max) y los deja
 * ordenados por nombre. Una carpeta que no existe no es error. Devuelve
 * cuántos agregó. */
int ps3_list_dols(const char *dir, int bundled, ps3_dol_t *list, int *count, int max);

/* La ruta del .map al lado de un .dol (misma ruta, otra extensión). */
void ps3_map_path(const char *dol_path, char *out, size_t size);

#endif
