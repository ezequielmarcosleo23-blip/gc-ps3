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
#include <stdio.h>

#include "ps3mapi_ps3_lib.h"
#include "core/version.h"
#include "common/harness.h"

static void log_env(const ps3_env_t *env)
{
    const char *kind = env->is_hen ? "HEN"
                     : env->is_mamba ? "Mamba"
                     : env->is_cobra ? "Cobra" : "sin CFW/HEN";

    LOGI("entorno: %s", kind);
    if (env->is_hen)
        LOGI("HEN rev: 0x%x", env->hen_revision);
    if (env->has_ps3mapi) {
        LOGI("PS3MAPI: 0x%04x (dynarec %s)", env->ps3mapi_core,
             ps3_env_supports_dynarec(env) ? "si" : "no");
        LOGI("firmware: %x.%02x %s", (env->fw_version >> 12) & 0xf,
             (env->fw_version >> 4) & 0xff, env->fw_type);
    } else {
        LOGI("PS3MAPI: no disponible");
    }
    LOGI("temperatura: CPU %u C, RSX %u C", env->cpu_temp_c, env->rsx_temp_c);
}

/* Señal sin pantalla; solo con HEN/CFW (en RPCS3 la syscall no existe). */
static void buzz(const ps3_env_t *env, int ok)
{
    if (!(env->is_hen || env->is_cobra || env->is_mamba))
        return;
    if (ok)
        ring_buzzer_simple();
    else
        ring_buzzer_triple();
}

int phase0_main(const char *name, phase0_test_fn fn)
{
    ps3_env_t env;

    udplog_init(name);
    LOGI("%s %s, prueba %s (compilada %s %s)", GCPS3_NAME, GCPS3_VERSION, name, __DATE__, __TIME__);
    LOGI("log UDP: %s", udplog_targets());

    ps3_env_detect(&env);
    log_env(&env);

    uint64_t t0 = udplog_now_us();
    int rc = fn(&env);
    uint64_t dt = udplog_now_us() - t0;

    LOGI("duracion de la prueba: %llu us", (unsigned long long)dt);
    LOGI("log: %u lineas enviadas, %u envios fallidos", udplog_sent() + 1, udplog_send_errors());
    udplog_result(rc == 0);
    buzz(&env, rc == 0);
    udplog_shutdown();
    return rc == 0 ? 0 : 1;
}
