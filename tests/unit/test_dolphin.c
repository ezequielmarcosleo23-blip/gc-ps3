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
 * Los DOLs de prueba contra Dolphin (docs/traza-dolphin.md): cada uno corre
 * con el reloj de su referencia (tests/dol/dolphin/<prueba>.txt) y la huella
 * encadenada del estado de la CPU, instrucción por instrucción, tiene que dar
 * lo mismo que el Dolphin parchado en cada punto (cada 65 536 instrucciones y
 * al final). Las referencias las genera tools/dolphin-trace/comparar.py.
 *
 * Si hay tests/dol/dolphin/<prueba>.gx.txt, el estado de GX al final (FIFO,
 * BP, CP y XF, src/gx/estado.c) también tiene que ser el de Dolphin.
 *
 * GCPS3_REF_MAX=N revisa solo hasta la instrucción N (make test-be, que corre
 * en QEMU, usa 2 millones); entonces no se mira el estado de GX.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "cpu/huella.h"
#include "test.h"

#define POINTS_MAX 4096

typedef struct point {
    uint64_t n;
    uint32_t pc;
    uint64_t chain;
} point_t;

static gc_system_t sys;
static point_t points[POINTS_MAX];
static int npoints, next_point, failed_point;
static uint64_t seen, chain;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void ignore_line(void *ctx, const char *line)
{
    (void)ctx;
    (void)line;
}

static void step(void *ctx, uint32_t pc, uint32_t inst)
{
    (void)ctx;
    inst = gc_hle_original(&sys, inst);
    chain = gekko_huella_cadena(chain, pc, inst, gekko_huella(&sys.cpu));
    seen++;
    if (next_point < npoints && seen == points[next_point].n) {
        const point_t *p = &points[next_point++];
        if (failed_point < 0 && (p->chain != chain || p->pc != pc)) {
            failed_point = next_point - 1;
            sys.cpu.stop = 1;
        }
    }
}

/* ---- estado de GX al final ---- */

#define ESTADO_MAX (256u * 1024u)

static char estado[ESTADO_MAX];
static size_t estado_len;

static void estado_line(void *ctx, const char *text)
{
    (void)ctx;
    const size_t n = strlen(text);
    if (text[0] != '#' && estado_len + n + 2 < ESTADO_MAX) {
        memcpy(estado + estado_len, text, n);
        estado_len += n;
        estado[estado_len++] = '\n';
        estado[estado_len] = '\0';
    }
}

/* 1 si no hay referencia o si el estado es el de Dolphin */
static int check_estado(const char *name)
{
    char path[256], line[256];
    snprintf(path, sizeof path, "tests/dol/dolphin/%s.gx.txt", name);
    FILE *f = fopen(path, "r");
    if (!f)
        return 1;
    static char want[ESTADO_MAX];
    size_t want_len = 0;
    while (fgets(line, sizeof line, f)) {
        const size_t n = strlen(line);
        if (line[0] != '#' && want_len + n + 1 < ESTADO_MAX) {
            memcpy(want + want_len, line, n);
            want_len += n;
        }
    }
    want[want_len] = '\0';
    fclose(f);

    /* Como el runner con --estado-gx: con los eventos de este ciclo atendidos */
    gc_sched_dispatch(&sys.sched, sys.cpu.cycles);
    estado_len = 0;
    estado[0] = '\0';
    gc_gx_estado(&sys, estado_line, NULL);
    if (strcmp(estado, want) == 0)
        return 1;
    size_t k = 0, at = 0;
    while (estado[k] && estado[k] == want[k]) {
        if (estado[k] == '\n')
            at = k + 1;
        k++;
    }
    fprintf(stderr, "  %s: estado de GX distinto de Dolphin desde\n    gc-ps3:  %.40s\n    Dolphin: %.40s\n",
            name, estado + at, want + at);
    return 0;
}

/* Lee la referencia: reloj e instrucciones de la cabecera y los puntos */
static int load_reference(const char *name, uint64_t *rtc)
{
    char path[256], line[256];
    snprintf(path, sizeof path, "tests/dol/dolphin/%s.txt", name);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "  %s: falta %s\n", name, path);
        return -1;
    }
    npoints = 0;
    *rtc = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long n, c, r;
        unsigned pc;
        if (sscanf(line, "# reloj %llu", &r) == 1)
            *rtc = r;
        else if (line[0] != '#' && sscanf(line, "%llu %x %llx", &n, &pc, &c) == 3 &&
                 npoints < POINTS_MAX)
            points[npoints++] = (point_t){ n, pc, c };
    }
    fclose(f);
    return npoints > 0 ? 0 : -1;
}

static int check_dol(const char *name)
{
    char path[256];
    uint64_t rtc, limit = UINT64_MAX;
    if (load_reference(name, &rtc) != 0)
        return 0;
    const char *max = getenv("GCPS3_REF_MAX");
    if (max)
        limit = strtoull(max, NULL, 0);
    const int all = npoints;
    while (npoints > 0 && points[npoints - 1].n > limit)
        npoints--;
    if (npoints == 0)
        return 1;

    snprintf(path, sizeof path, "tests/dol/bin/%s.dol", name);
    FILE *f = fopen(path, "rb");
    uint8_t *dol = malloc(1u << 20);
    const size_t dol_len = f && dol ? fread(dol, 1, 1u << 20, f) : 0;
    if (f)
        fclose(f);
    snprintf(path, sizeof path, "tests/dol/bin/%s.map", name);
    f = fopen(path, "rb");
    char *map = malloc(1u << 20);
    const size_t map_len = f && map ? fread(map, 1, 1u << 20, f) : 0;
    if (f)
        fclose(f);

    int ok = 0;
    gc_log_set_sink(quiet);
    const gc_boot_config_t cfg = { 0, rtc };
    if (dol_len && map_len && gc_system_init(&sys) == 0) {
        if (gc_system_boot_dol(&sys, dol, dol_len, &cfg) == GC_DOL_OK) {
            gc_system_load_map(&sys, map, map_len);
            sys.hle.report = ignore_line;
            sys.cpu.trace = step;
            seen = chain = 0;
            next_point = 0;
            failed_point = -1;
            gc_system_run(&sys, points[npoints - 1].n);
            ok = failed_point < 0 && next_point == npoints;
            if (ok && npoints == all)
                ok = check_estado(name);
            if (failed_point >= 0) {
                const uint64_t from = failed_point ? points[failed_point - 1].n : 0;
                fprintf(stderr, "  %s: distinto de Dolphin entre las instrucciones %llu y %llu "
                        "(tools/dolphin-trace/comparar.py dice dónde)\n", name,
                        (unsigned long long)from, (unsigned long long)points[failed_point].n);
            } else if (!ok) {
                fprintf(stderr, "  %s: paró en la instrucción %llu, Dolphin llega a %llu\n", name,
                        (unsigned long long)seen, (unsigned long long)points[npoints - 1].n);
            }
        }
        gc_system_free(&sys);
    }
    gc_log_set_sink(NULL);
    free(dol);
    free(map);
    return ok;
}

/* El formato de la huella (v1), con valores calculados aparte en Python: el
 * parche de Dolphin calcula lo mismo */
static void test_huella_format(void)
{
    static gekko_t c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 32; i++) {
        c.gpr[i] = (uint32_t)i * 0x01010101u;
        c.fpr[i][0] = (uint64_t)i << 56 | 1;
        c.fpr[i][1] = (uint64_t)i << 56 | 2;
    }
    c.cr = 0x12345678u;
    c.xer = 0x20000000u;
    c.lr = 0x80003100u;
    c.ctr = 7;
    c.msr = 0x2032u;
    c.fpscr = 4;
    c.spr[SPR_SRR0] = 0x800;
    c.spr[SPR_SRR1] = 0x1032;
    for (int i = 0; i < 8; i++)
        c.spr[SPR_GQR0 + i] = (uint32_t)i << 16 | (uint32_t)i;
    c.spr[SPR_HID0] = 0x0011C464u;
    c.spr[SPR_HID2] = 0xE0000000u;
    const uint64_t h = gekko_huella(&c);
    CHECK_EQ_U64(h, 0x3aa104940444f8deull);
    CHECK_EQ_U64(gekko_huella_cadena(0, 0x80003100u, 0x7c0802a6u, h), 0x5e70447fdbbc56cdull);
    /* Cada registro cuenta */
    c.spr[SPR_HID2] ^= 1;
    CHECK(gekko_huella(&c) != h);
    c.spr[SPR_HID2] ^= 1;
    c.fpr[31][1] ^= 1ull << 63;
    CHECK(gekko_huella(&c) != h);
}

static void test_hola(void)     { CHECK(check_dol("hola")); }
static void test_cpu(void)      { CHECK(check_dol("cpu")); }
static void test_timer(void)    { CHECK(check_dol("timer")); }
static void test_hw(void)       { CHECK(check_dol("hw")); }
static void test_pantalla(void) { CHECK(check_dol("pantalla")); }
static void test_contador(void) { CHECK(check_dol("contador")); }
static void test_gx_fifo(void)  { CHECK(check_dol("gx_fifo")); }

const test_case_t dolphin_tests[] = {
    { "huella: formato v1 (el mismo del parche de Dolphin)", test_huella_format },
    { "dolphin hola: cada instrucción igual a Dolphin", test_hola },
    { "dolphin cpu: cada instrucción igual a Dolphin", test_cpu },
    { "dolphin timer: cada instrucción igual a Dolphin", test_timer },
    { "dolphin hw: cada instrucción igual a Dolphin", test_hw },
    { "dolphin pantalla: 30 millones de instrucciones iguales a Dolphin", test_pantalla },
    { "dolphin contador: 30 millones de instrucciones iguales a Dolphin", test_contador },
    { "dolphin gx_fifo: cada instrucción y el estado de GX iguales a Dolphin", test_gx_fifo },
    { NULL, NULL },
};
