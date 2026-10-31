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
 * Fase 2, etapa D3: el TEV como fragment program (src/gx/tevfp.c). Los DOLs
 * que dibujan corren con el rasterizador por software y, en cada píxel que
 * llega al TEV, el programa de ese estado corre en el simulador con las mismas
 * entradas (colores, coordenadas, Z, x) y el muestreador de tex.c: el color,
 * la prueba de alfa y la textura de Z tienen que dar lo mismo. Con niebla se
 * admite 1 de diferencia por canal (EX2, LG2 y RCP); en la Z de la textura,
 * 1 (la vuelta por un float de 0 a 1). Lo que Dolphin lee de otro píxel no
 * se compara (tevfp.h). De cada programa se comparan los primeros 4096
 * píxeles y después 1 de cada 16. gx_copy no está: sus 2 programas son los de
 * los otros DOLs y tarda. Al final, una línea por DOL en la salida de error.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "gx/tev.h"
#include "gx/tevfp.h"
#include "test.h"

#define MAX_CYCLES (200u * 1000u * 1000u)
#define CACHE      1024
#define SHOW       5                            /* diferencias que se imprimen por DOL */

static gc_system_t sys;

typedef struct entry {
    gx_tevfp_key_t key;
    gx_tevfp_t    *p;
    int            built;
    uint64_t       seen;
} entry_t;

static entry_t  cache[CACHE];
static uint32_t ncache;

typedef struct stats {
    uint64_t pixels, skipped, exact, fog_near, stale, bad;
    uint32_t stale_why[4];
    uint32_t programs, failed, overflow, shown;
    uint32_t max_words, max_insn;
} stats_t;

static stats_t st;
static const gx_tev_probe_t *cur_probe;
static const gx_tevfp_t *cur_prog;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

static void tex_cb(void *ctx, uint32_t ord, uint32_t unit, const float coord[4], float out[4])
{
    (void)ctx;
    (void)unit;
    const gx_tevfp_t *p = cur_prog;
    const gx_tev_probe_t *pr = cur_probe;
    if (ord >= p->ntex) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    const uint32_t s = p->tex[ord].stage;
    const int32_t lod = p->tex[ord].ind ? pr->ind_lod[s] : pr->tex_lod[s];
    const int linear = p->tex[ord].ind ? pr->ind_linear[s] : pr->tex_linear[s];
    uint8_t t[4];
    gx_tex_sample(&sys.gx, (int32_t)coord[0], (int32_t)coord[1], lod, linear, p->tex[ord].texmap, t);
    for (int c = 0; c < 4; c++)
        out[c] = (float)t[c] / 255.0f;
}

static entry_t *program(const uint32_t *bp)
{
    gx_tevfp_key_t key;
    gx_tevfp_key(bp, &key);
    const uint32_t h = gx_tevfp_key_hash(&key);
    for (uint32_t n = 0, i = h % CACHE; n < CACHE; n++, i = (i + 1) % CACHE) {
        entry_t *e = &cache[i];
        if (e->p && memcmp(&e->key, &key, sizeof key) == 0)
            return e;
        if (!e->p) {
            if (ncache >= CACHE - 1)
                return NULL;
            e->key = key;
            e->p = malloc(sizeof *e->p);
            if (!e->p)
                return NULL;
            ncache++;
            st.programs++;
            e->built = gx_tevfp_build(bp, e->p) == 0;
            if (!e->built)
                st.failed++;
            else {
                if (e->p->info.nwords > st.max_words)
                    st.max_words = e->p->info.nwords;
                if (e->p->info.ninsn > st.max_insn)
                    st.max_insn = e->p->info.ninsn;
            }
            return e;
        }
    }
    return NULL;
}

static void show(const char *what, const gx_tev_probe_t *pr, const rsxfp_out_t *o)
{
    if (st.shown++ >= SHOW)
        return;
    fprintf(stderr, "    %s en (%d, %d): TEV %s %02x%02x%02x%02x z %06x; programa %s %.3f %.3f %.3f %.3f z %.1f\n",
            what, (int)pr->pos[0], (int)pr->pos[1], pr->killed ? "KIL" : "", pr->out[0], pr->out[1], pr->out[2],
            pr->out[3], (unsigned)pr->z, o->killed ? "KIL" : "", o->color[0] * 255.0f, o->color[1] * 255.0f,
            o->color[2] * 255.0f, o->color[3] * 255.0f, o->depth * 16777215.0f);
}

static void probe(void *ctx, const gx_tev_probe_t *pr)
{
    (void)ctx;
    st.pixels++;
    entry_t *e = program(sys.gx.bp);
    if (!e) {
        st.overflow++;
        return;
    }
    if (!e->built) {
        st.bad++;
        return;
    }
    const gx_tevfp_t *p = e->p;
    if (e->seen++ >= 4096 && e->seen % 16 != 0) {
        st.skipped++;
        return;
    }
    if (p->stale) {
        st.stale++;
        for (int k = 0; k < 4; k++)
            if (p->stale & (1u << k))
                st.stale_why[k]++;
        return;
    }
    gx_tevfp_patch(&sys.gx, p, e->p->words, 1);

    rsxfp_env_t env;
    memset(&env, 0, sizeof env);
    for (int i = 0; i < 8; i++) {
        env.in[RSXFP_TC0 + i][0] = (float)pr->uv[i][0] / 128.0f;
        env.in[RSXFP_TC0 + i][1] = (float)pr->uv[i][1] / 128.0f;
        env.in[RSXFP_TC0 + i][2] = 1.0f;
        env.in[RSXFP_TC0 + i][3] = (float)pr->color[i >> 2][i & 3];
    }
    env.in[RSXFP_FOGC][0] = (float)pr->pos[2];
    env.in[RSXFP_WPOS][0] = (float)pr->pos[0] + 0.5f;
    env.in[RSXFP_WPOS][1] = (float)pr->pos[1] + 0.5f;
    env.tex = tex_cb;
    cur_probe = pr;
    cur_prog = p;
    rsxfp_out_t o;
    if (rsxfp_run(p->words, p->info.nwords, &env, &o) != 0) {
        st.bad++;
        show("no corre", pr, &o);
        return;
    }
    if (o.killed != pr->killed) {
        st.bad++;
        show("prueba de alfa", pr, &o);
        return;
    }
    if (pr->killed) {
        st.exact++;
        return;
    }
    int worst = 0;
    for (int c = 0; c < 4; c++) {
        const int v = (int)lrintf(o.color[c] * 255.0f);
        const int d = abs(v - pr->out[c]);
        if (d > worst)
            worst = d;
    }
    if (p->ztex) {
        const long z = lrint((double)o.depth * 16777215.0);
        if (labs(z - pr->z) > 1) {
            st.bad++;
            show("Z", pr, &o);
            return;
        }
    }
    if (worst == 0)
        st.exact++;
    else if (p->fog && worst <= 1)
        st.fog_near++;
    else {
        st.bad++;
        show("color", pr, &o);
    }
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

static void free_cache(void)
{
    for (uint32_t i = 0; i < CACHE; i++) {
        free(cache[i].p);
        cache[i].p = NULL;
    }
    ncache = 0;
}

/* Corre el DOL con la comparación en cada píxel; 1 si no hubo diferencias */
static int check_dol(const char *name)
{
    char path[256];
    size_t dol_len, map_len;
    snprintf(path, sizeof path, "tests/dol/bin/%s.dol", name);
    char *dol = slurp(path, &dol_len);
    snprintf(path, sizeof path, "tests/dol/bin/%s.map", name);
    char *map = slurp(path, &map_len);
    int ok = 0;
    memset(&st, 0, sizeof st);
    uint64_t cycles = MAX_CYCLES;
    const char *max = getenv("GCPS3_REF_MAX");     /* en QEMU, solo el principio */
    if (max)
        cycles = strtoull(max, NULL, 0);
    gc_log_set_sink(quiet);
    if (dol && map && gc_system_init(&sys) == 0) {
        const gc_boot_config_t cfg = { 0, 0 };
        if (gc_system_boot_dol(&sys, (const uint8_t *)dol, dol_len, &cfg) == GC_DOL_OK) {
            gc_system_load_map(&sys, map, map_len);
            sys.gx.tev_probe = probe;
            gc_system_run(&sys, cycles);
            ok = st.bad == 0 && st.overflow == 0 && st.failed == 0 && st.pixels > 0;
        }
        gc_system_free(&sys);
    }
    gc_log_set_sink(NULL);
    fprintf(stderr, "  tevfp %s: %llu píxeles (%llu sin comparar), %llu iguales, %llu con niebla a 1, %llu con "
            "estado de otro píxel (textura %u, color %u, indirecta %u, coordenada %u), %llu distintos; %u programas "
            "(%u sin armar), hasta %u instrucciones y %u palabras\n", name, (unsigned long long)st.pixels,
            (unsigned long long)st.skipped, (unsigned long long)st.exact, (unsigned long long)st.fog_near,
            (unsigned long long)st.stale, (unsigned)st.stale_why[0], (unsigned)st.stale_why[1],
            (unsigned)st.stale_why[2], (unsigned)st.stale_why[3], (unsigned long long)st.bad,
            (unsigned)st.programs, (unsigned)st.failed, (unsigned)st.max_insn, (unsigned)st.max_words);
    free_cache();
    free(dol);
    free(map);
    return ok;
}

static void test_tevfp_raster(void) { CHECK(check_dol("gx_raster")); }
static void test_tevfp_tex(void) { CHECK(check_dol("gx_tex")); }
static void test_tevfp_tev(void) { CHECK(check_dol("gx_tev")); }
static void test_tevfp_escena(void) { CHECK(check_dol("gx_escena")); }

/* La clave: lo que cambia el programa la cambia; las constantes, no */
static void test_tevfp_key(void)
{
    uint32_t bp[256];
    memset(bp, 0, sizeof bp);
    gx_tevfp_key_t a, b;
    gx_tevfp_key(bp, &a);
    bp[0xE0] = 0x123;                           /* un registro del TEV */
    bp[0xF3] = 0x00002080;                      /* referencias de la prueba de alfa */
    bp[0xF4] = 0x456;                           /* sesgo de la textura de Z */
    bp[0x06] = 0x7FF;                           /* una matriz indirecta */
    gx_tevfp_key(bp, &b);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
    bp[0xC0] = 1;                               /* un combinador */
    gx_tevfp_key(bp, &b);
    CHECK(memcmp(&a, &b, sizeof a) != 0);
    CHECK(gx_tevfp_key_hash(&a) != gx_tevfp_key_hash(&b));
}

const test_case_t tevfp_tests[] = {
    { "tevfp: la clave del programa", test_tevfp_key },
    { "tevfp gx_raster: cada píxel del programa igual al TEV", test_tevfp_raster },
    { "tevfp gx_tex: cada píxel del programa igual al TEV", test_tevfp_tex },
    { "tevfp gx_tev: cada píxel del programa igual al TEV", test_tevfp_tev },
    { "tevfp gx_escena: cada píxel del programa igual al TEV", test_tevfp_escena },
    { NULL, NULL },
};
