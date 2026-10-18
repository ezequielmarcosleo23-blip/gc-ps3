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
#include <string.h>
#include <ppu-types.h>

#include "ps3mapi_ps3_lib.h"
#include "platform/ps3/env.h"

void ps3_env_detect(ps3_env_t *env)
{
    memset(env, 0, sizeof *env);

    int core = ps3mapi_get_core_version();
    env->ps3mapi_core = core > 0 ? core : 0;
    env->has_ps3mapi  = env->ps3mapi_core >= PS3MAPI_CORE_MINVERSION;

    if (env->has_ps3mapi) {
        env->fw_version = ps3mapi_get_fw_version();
        if (ps3mapi_get_fw_type(env->fw_type) < 0)
            env->fw_type[0] = '\0';
        env->fw_type[sizeof env->fw_type - 1] = '\0';
    }

    env->is_hen = is_ps3hen();
    if (env->is_hen)
        env->hen_revision = sys8_get_hen_version();
    env->is_cobra = is_cobra();
    env->is_mamba = is_mamba();

    get_temperature_celcius(&env->cpu_temp_c, &env->rsx_temp_c);
}

int ps3_env_supports_dynarec(const ps3_env_t *env)
{
    return env->has_ps3mapi && env->ps3mapi_core >= GCPS3_PS3MAPI_DYNAREC_MIN;
}

void ps3_env_print(const ps3_env_t *env, FILE *out)
{
    fprintf(out, "entorno: %s%s%s\n",
            env->is_hen ? "HEN " : "",
            env->is_mamba ? "Mamba " : (env->is_cobra ? "Cobra " : ""),
            (!env->is_hen && !env->is_cobra && !env->is_mamba) ? "sin CFW/HEN" : "");
    if (env->is_hen)
        fprintf(out, "HEN rev: 0x%x\n", env->hen_revision);
    if (env->has_ps3mapi) {
        fprintf(out, "PS3MAPI: 0x%04x (dynarec %s)\n", env->ps3mapi_core,
                ps3_env_supports_dynarec(env) ? "si" : "no");
        fprintf(out, "firmware: %x.%02x %s\n", (env->fw_version >> 12) & 0xf,
                (env->fw_version >> 4) & 0xff, env->fw_type);
    } else {
        fprintf(out, "PS3MAPI: no disponible\n");
    }
    fprintf(out, "temperatura: CPU %u C, RSX %u C\n", env->cpu_temp_c, env->rsx_temp_c);
}
