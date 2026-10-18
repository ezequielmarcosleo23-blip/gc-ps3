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

/* Detección del entorno de la consola (HEN / Cobra / Mamba, PS3MAPI, firmware). */
#ifndef GCPS3_PLATFORM_PS3_ENV_H
#define GCPS3_PLATFORM_PS3_ENV_H

#include <stdint.h>
#include <stdio.h>

/* Versión mínima del núcleo PS3MAPI que anuncia soporte de dynarec. */
#define GCPS3_PS3MAPI_DYNAREC_MIN 0x0124

typedef struct ps3_env {
    int      has_ps3mapi;
    int      ps3mapi_core;      /* p. ej. 0x0124 */
    int      fw_version;        /* p. ej. 0x4920 = 4.92 */
    char     fw_type[32];       /* "CEX", "DEX", ... */
    int      is_hen;
    int      hen_revision;
    int      is_cobra;
    int      is_mamba;
    uint32_t cpu_temp_c;
    uint32_t rsx_temp_c;
} ps3_env_t;

void ps3_env_detect(ps3_env_t *env);
void ps3_env_print(const ps3_env_t *env, FILE *out);

/* 1 si PS3MAPI es lo bastante nuevo para el JIT (>= 0x124). */
int ps3_env_supports_dynarec(const ps3_env_t *env);

#endif
