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
 * .map, la CPU, el hardware y la salida. Cada uno tiene que imprimir
 * exactamente tests/dol/esperado/<prueba>.txt: los que terminan, con exit(0);
 * los que se quedan mostrando algo (pantalla, contador), sin terminar dentro
 * de sus ciclos.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "test.h"

#define MAX_CYCLES (200u * 1000u * 1000u)
#define OUT_MAX    8192
#define STAYS      0            /* el programa no termina: corre sus ciclos */
#define EXITS      1

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

/* Último campo impar y par que informó la VI */
static gc_vi_field_t field_odd, field_even;

static void on_field(void *ctx, const gc_vi_field_t *f)
{
    (void)ctx;
    if (f->even)
        field_even = *f;
    else
        field_odd = *f;
}

static gc_system_t sys;

/* CRC-32 (zlib) de un cuadro entrelazado tal como lo recorre la VI: la línea
 * 2k del campo impar y la 2k+1 del par. 0 si la geometría no es 640x480. */
static uint32_t frame_crc32(void)
{
    if (field_odd.width != 640 || field_odd.height != 240 || field_even.width != 640 ||
        field_even.height != 240 || field_odd.stride != field_even.stride)
        return 0;
    uint32_t table[256], crc = 0xFFFFFFFFu;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    for (uint32_t line = 0; line < 480; line++) {
        const gc_vi_field_t *f = (line & 1) ? &field_even : &field_odd;
        const uint8_t *p = gc_ram_ptr(&sys.mem, f->xfb + (line / 2) * f->stride, 640 * 2);
        if (!p)
            return 0;
        for (uint32_t i = 0; i < 640 * 2; i++)
            crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* pantalla: el CRC que imprimió el programa es el del cuadro que mostró la VI */
static int check_screen(const char *name)
{
    unsigned printed = 0;
    const char *at = strstr(out, "crc32 del XFB 0x");
    if (!at || sscanf(at, "crc32 del XFB 0x%x", &printed) != 1)
        return 0;
    const uint32_t seen = frame_crc32();
    if (seen != printed)
        fprintf(stderr, "  %s: la VI mostro un cuadro con CRC 0x%08x (XFB 0x%08x/0x%08x, %ux%u)\n",
                name, (unsigned)seen, (unsigned)field_odd.xfb, (unsigned)field_even.xfb,
                (unsigned)field_odd.width, (unsigned)field_odd.height);
    return seen == printed && field_odd.xfb == 0x00200000u && field_even.xfb == 0x00200500u;
}

/* 1 si el DOL corrió, terminó como tenía que terminar e imprimió lo esperado. */
static int run_program(const char *name, uint64_t cycles, int exits)
{
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
    memset(&field_odd, 0, sizeof field_odd);
    memset(&field_even, 0, sizeof field_even);
    sys.vi.on_field = on_field;
    const uint64_t ran = gc_system_run(&sys, cycles);
    gc_hle_flush(&sys);

    const int ended = exits ? sys.hle.exited && sys.hle.exit_code == 0 : !sys.hle.exited;
    ok = hooks >= 4 && ended && sys.cpu.illegal == 0 && strcmp(out, want) == 0;
    if (ok && strcmp(name, "pantalla") == 0)
        ok = check_screen(name);
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

static void test_hola(void)     { CHECK(run_program("hola", MAX_CYCLES, EXITS)); }
static void test_cpu(void)      { CHECK(run_program("cpu", MAX_CYCLES, EXITS)); }
static void test_timer(void)    { CHECK(run_program("timer", MAX_CYCLES, EXITS)); }
/* Termina de imprimir a los ~26 M ciclos y ~73 M: un cuadro (16,2 M) de margen */
static void test_pantalla(void) { CHECK(run_program("pantalla", 45000000u, STAYS)); }
static void test_contador(void) { CHECK(run_program("contador", 90000000u, STAYS)); }

const test_case_t program_tests[] = {
    { "dol hola: OSReport, printf con pila y va_list, puts (= salida en la PC)", test_hola },
    { "dol cpu: C compilado + paired singles, psq, fres, fmadds (= salida en la PC)", test_cpu },
    { "dol timer: decrementador en 0x900 con MSR[EE], 10 interrupciones", test_timer },
    { "dol pantalla: hola mundo en el XFB, VI 480i, CRC del cuadro mostrado", test_pantalla },
    { "dol contador: interrupción de VI en 0x500, 59,94 campos por segundo", test_contador },
    { NULL, NULL },
};
