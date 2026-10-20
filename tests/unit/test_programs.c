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
 * Los DOLs de prueba (tests/dol) de punta a punta: arranque HLE, ganchos del
 * .map, la CPU y la salida. Cada uno tiene que terminar con exit(0) e imprimir
 * exactamente tests/dol/esperado/<prueba>.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "test.h"

#define MAX_CYCLES (200u * 1000u * 1000u)
#define OUT_MAX    8192

static char out[OUT_MAX];
static size_t out_len;

static void collect(void *ctx, const char *line)
{
    (void)ctx;
    const size_t n = strlen(line);
    if (out_len + n + 2 < OUT_MAX) {
        memcpy(out + out_len, line, n);
        out_len += n;
        out[out_len++] = '\n';
        out[out_len] = '\0';
    }
}

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = '\0';
        *len = (size_t)n;
    }
    return buf;
}

/* 1 si el DOL corrió, terminó con exit(0) e imprimió lo esperado. */
static int run_program(const char *name)
{
    static gc_system_t sys;
    char path[256];
    size_t dol_len = 0, map_len = 0, want_len = 0;
    int ok = 0;

    snprintf(path, sizeof path, "tests/dol/bin/%s.dol", name);
    char *dol = slurp(path, &dol_len);
    snprintf(path, sizeof path, "tests/dol/bin/%s.map", name);
    char *map = slurp(path, &map_len);
    snprintf(path, sizeof path, "tests/dol/esperado/%s.txt", name);
    char *want = slurp(path, &want_len);
    if (!dol || !map || !want) {
        fprintf(stderr, "  %s: faltan archivos en tests/dol\n", name);
        goto done;
    }

    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        goto done;
    const gc_boot_config_t cfg = { 0, 0 };
    if (gc_system_boot_dol(&sys, (const uint8_t *)dol, dol_len, &cfg) != GC_DOL_OK) {
        fprintf(stderr, "  %s: no carga\n", name);
        gc_system_free(&sys);
        goto done;
    }
    const int hooks = gc_system_load_map(&sys, map, map_len);
    sys.hle.report = collect;
    out_len = 0;
    out[0] = '\0';
    const uint64_t ran = gc_system_run(&sys, MAX_CYCLES);
    gc_hle_flush(&sys);

    ok = hooks >= 4 && sys.hle.exited && sys.hle.exit_code == 0 && sys.cpu.illegal == 0 &&
         strcmp(out, want) == 0;
    if (!ok)
        fprintf(stderr, "  %s: %llu ciclos, exit %s %u, %u ilegales, %d ganchos; salida:\n%s",
                name, (unsigned long long)ran, sys.hle.exited ? "si" : "no", (unsigned)sys.hle.exit_code,
                (unsigned)sys.cpu.illegal, hooks, out);
    gc_system_free(&sys);
done:
    gc_log_set_sink(NULL);
    free(dol);
    free(map);
    free(want);
    return ok;
}

static void test_hola(void)  { CHECK(run_program("hola")); }
static void test_cpu(void)   { CHECK(run_program("cpu")); }
static void test_timer(void) { CHECK(run_program("timer")); }

const test_case_t program_tests[] = {
    { "dol hola: OSReport, printf con pila y va_list, puts (= salida en la PC)", test_hola },
    { "dol cpu: C compilado + paired singles, psq, fres, fmadds (= salida en la PC)", test_cpu },
    { "dol timer: decrementador en 0x900 con MSR[EE], 10 interrupciones", test_timer },
    { NULL, NULL },
};
