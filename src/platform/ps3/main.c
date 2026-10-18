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
 * App de PS3 de la etapa A: demuestra que el build produce un .self que
 * detecta el entorno y lanza un programa SPE incrustado. La salida va a
 * stdout (TTY); en la etapa B se agrega el log por UDP.
 */
#include <stdio.h>
#include <string.h>
#include <ppu-types.h>
#include <sys/spu.h>

#include "core/version.h"
#include "platform/ps3/env.h"
#include "spu/hello/job.h"
#include "spu_hello_bin.h"

#define PTR2EA(x) ((u64)(uintptr_t)(x))

static hello_job_t job;

static int run_spu_hello(void)
{
    static const float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    static const float b[4] = { 0.5f, 2.0f, -1.0f, 10.0f };
    static const float expected[4] = { 1.5f, 6.0f, 0.0f, 44.0f };

    sysSpuImage image;
    sysSpuThreadGroupAttribute group_attr;
    sysSpuThreadAttribute thread_attr;
    sysSpuThreadArgument arg;
    u32 group_id, thread_id, cause = 0, status = 0;
    s32 rc;

    memset(&job, 0, sizeof job);
    memcpy(job.a, a, sizeof a);
    memcpy(job.b, b, sizeof b);

    sysSpuThreadGroupAttributeInitialize(group_attr);
    sysSpuThreadGroupAttributeName(group_attr, "gcps3_hello");
    sysSpuThreadAttributeInitialize(thread_attr);
    sysSpuThreadAttributeName(thread_attr, "hello");
    sysSpuThreadArgumentInitialize(arg);
    arg.arg0 = PTR2EA(&job);

    if ((rc = sysSpuImageImport(&image, spu_hello_bin, SPU_IMAGE_PROTECT)) != 0) {
        printf("spu: sysSpuImageImport fallo (0x%08x)\n", rc);
        return -1;
    }
    if ((rc = sysSpuThreadGroupCreate(&group_id, 1, 100, &group_attr)) != 0) {
        printf("spu: sysSpuThreadGroupCreate fallo (0x%08x)\n", rc);
        sysSpuImageClose(&image);
        return -1;
    }
    if ((rc = sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &thread_attr, &arg)) != 0) {
        printf("spu: sysSpuThreadInitialize fallo (0x%08x)\n", rc);
        sysSpuThreadGroupDestroy(group_id);
        sysSpuImageClose(&image);
        return -1;
    }

    sysSpuThreadGroupStart(group_id);
    sysSpuThreadGroupJoin(group_id, &cause, &status);
    sysSpuThreadGroupDestroy(group_id);
    sysSpuImageClose(&image);

    __asm__ volatile ("sync" ::: "memory");
    if (!job.done) {
        printf("spu: el SPE no marco el trabajo como terminado (cause=%u status=%u)\n", cause, status);
        return -1;
    }
    for (int i = 0; i < 4; i++) {
        if (job.out[i] != expected[i]) {
            printf("spu: out[%d] = %f, esperado %f\n", i, (double)job.out[i], (double)expected[i]);
            return -1;
        }
    }
    printf("spu: hello OK (%.1f %.1f %.1f %.1f)\n",
           (double)job.out[0], (double)job.out[1], (double)job.out[2], (double)job.out[3]);
    return 0;
}

int main(void)
{
    ps3_env_t env;

    printf("%s %s\n", GCPS3_NAME, GCPS3_VERSION);

    ps3_env_detect(&env);
    ps3_env_print(&env, stdout);

    sysSpuInitialize(6, 0);
    int ok = run_spu_hello() == 0;

    printf("RESULT: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
