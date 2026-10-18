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

/* Las interfaces de include/backend deben compilar igual en la PC que en la PS3. */
#include <stddef.h>

#include "backend/gfx_backend.h"
#include "backend/spu_jobs.h"
#include "test.h"

static void test_spu_job_desc_layout(void)
{
    CHECK_EQ_U64(sizeof(spu_job_desc_t), 128);
    CHECK_EQ_U64(_Alignof(spu_job_desc_t), 128);
    CHECK_EQ_U64(offsetof(spu_job_desc_t, in_ea), 8);
    CHECK_EQ_U64(offsetof(spu_job_desc_t, done_ea), 32);
}

static void test_gfx_backend_ops(void)
{
    gfx_backend_t be = { NULL, NULL };
    CHECK(be.ops == NULL && be.priv == NULL);
}

const test_case_t backend_tests[] = {
    { "backend: descriptor SPU de 128 B", test_spu_job_desc_layout },
    { "backend: interfaz gráfica compila", test_gfx_backend_ops },
    { NULL, NULL },
};
