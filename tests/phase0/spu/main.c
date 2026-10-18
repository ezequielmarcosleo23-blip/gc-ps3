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
 * Prueba "spu" (etapa C): 64 000 vértices x matriz 4x4 en los SPE, con DMA de
 * doble búfer, comparados contra el PPU.
 *
 *   raw        un hilo SPU directo por ejecución (crear grupo, correr, destruir)
 *   kit x1     un trabajador persistente de spukit
 *   kit x4     cuatro trabajadores de spukit, un cuarto de los vértices cada uno
 *
 * Con valores exactos (enteros y fracciones de potencia de 2) el resultado no
 * depende del redondeo y el SPU debe dar los mismos bits que el PPU: eso decide
 * OK/FAIL. Después, con valores cualquiera, se informa cuántos componentes
 * difieren y el error máximo relativo a la escala de la cuenta (la SPU
 * redondea hacia cero; el PPU, al más cercano): es el dato que respalda la decisión D-007 (FPU de Gekko en el PPE).
 */
#include <malloc.h>
#include <math.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/spu.h>
#include <sys/systime.h>
#include <spukit/spukit.h>

#include "common/harness.h"
#include "spu/xform/job.h"
#include "spu_xform_bin.h"
#include "spu_xform_kit_bin.h"

#define NVERT    64000u
#define RUNS     5
#define WORKERS  4
#define PTR2EA(x) ((u64)(uintptr_t)(x))

typedef float vert_t[4];

static vert_t *src, *dst, *ref;
static xform_raw_job_t raw_job;

/* Enteros y fracciones de potencia de 2: todos los productos y sumas son exactos. */
static const float m_exact[16] = {
     2.0f,  -1.0f,   0.5f,   3.0f,
     0.25f,  1.0f,  -2.0f,  -0.5f,
    -1.5f,   0.75f,  1.0f,   2.0f,
     0.0f,   0.0f,   0.0f,   1.0f,
};

/* Valores cualquiera: aquí el redondeo sí cuenta. */
static const float m_any[16] = {
     0.8660254f, -0.5f,       0.1f,       0.3f,
     0.5f,        0.8660254f, -0.2f,       0.7f,
     0.1234567f,  0.7654321f,  0.9f,      -0.33f,
     0.01f,       0.02f,       0.03f,      1.0f,
};

static uint32_t rng = 0x2545F491u;

static uint32_t xorshift(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void fill_exact(void)
{
    for (uint32_t i = 0; i < NVERT; i++) {
        src[i][0] = (float)((int)(xorshift() % 1024) - 512);
        src[i][1] = (float)((int)(xorshift() % 1024) - 512);
        src[i][2] = (float)((int)(xorshift() % 1024) - 512);
        src[i][3] = 1.0f;
    }
}

static void fill_any(void)
{
    for (uint32_t i = 0; i < NVERT; i++) {
        for (int k = 0; k < 3; k++)                 /* [-1, 1) con 24 bits */
            src[i][k] = (float)(xorshift() >> 8) * (2.0f / 16777216.0f) - 1.0f;
        src[i][3] = 1.0f;
    }
}

/* Referencia en el PPU, en el mismo orden que el SPU: x, luego y, z, w con fmaf. */
static void ppu_xform(const float *m)
{
    for (uint32_t i = 0; i < NVERT; i++) {
        const float x = src[i][0], y = src[i][1], z = src[i][2], w = src[i][3];
        for (int r = 0; r < 4; r++) {
            float acc = m[r * 4 + 0] * x;
            acc = fmaf(m[r * 4 + 1], y, acc);
            acc = fmaf(m[r * 4 + 2], z, acc);
            acc = fmaf(m[r * 4 + 3], w, acc);
            ref[i][r] = acc;
        }
    }
}

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/*
 * Componentes distintos de la referencia y el mayor error, medido contra la
 * escala de cada cuenta: |spu - ppu| / (|m0 x| + |m1 y| + |m2 z| + |m3 w|), en
 * unidades de 2^-24 (el redondeo de un float). Las ULP del resultado no sirven
 * aquí: cerca de cero, por cancelación, una diferencia de redondeo en los
 * términos se ve como miles de ULP del resultado.
 */
static uint32_t compare(const float *m, double *max_units)
{
    uint32_t bad = 0;
    *max_units = 0.0;
    for (uint32_t i = 0; i < NVERT; i++)
        for (int k = 0; k < 4; k++) {
            if (bits(dst[i][k]) == bits(ref[i][k]))
                continue;
            bad++;
            double scale = 0.0;
            for (int j = 0; j < 4; j++)
                scale += fabs((double)m[k * 4 + j] * (double)src[i][j]);
            double diff = fabs((double)dst[i][k] - (double)ref[i][k]);
            double units = scale > 0.0 ? diff / (scale * 0x1p-24) : 1e30;
            if (!(units <= *max_units))          /* NaN también cuenta */
                *max_units = units;
        }
    return bad;
}

static void params(xform_params_t *p, const float *m, uint32_t first, uint32_t count)
{
    memset(p, 0, sizeof *p);
    memcpy(p->m, m, sizeof p->m);
    p->src_ea = PTR2EA(src + first);
    p->dst_ea = PTR2EA(dst + first);
    p->count = count;
}

static double tb_to_us(uint64_t ticks)
{
    return (double)ticks * 1e6 / (double)sysGetTimebaseFrequency();
}

/* ---- hilo SPU directo ---------------------------------------------------- */

static int run_raw(const float *m, uint64_t *wall_us, uint32_t *spu_ticks)
{
    sysSpuImage image;
    sysSpuThreadGroupAttribute group_attr;
    sysSpuThreadAttribute thread_attr;
    sysSpuThreadArgument arg;
    u32 group_id, thread_id, cause = 0, status = 0;
    s32 rc;

    memset(&raw_job, 0, sizeof raw_job);
    params(&raw_job.p, m, 0, NVERT);
    __asm__ volatile ("sync" ::: "memory");

    const uint64_t t0 = udplog_now_us();
    sysSpuThreadGroupAttributeInitialize(group_attr);
    sysSpuThreadGroupAttributeName(group_attr, "gcps3_xform");
    sysSpuThreadAttributeInitialize(thread_attr);
    sysSpuThreadAttributeName(thread_attr, "xform");
    sysSpuThreadArgumentInitialize(arg);
    arg.arg0 = PTR2EA(&raw_job);

    if ((rc = sysSpuImageImport(&image, spu_xform_bin, SPU_IMAGE_PROTECT)) != 0) {
        LOGE("raw: sysSpuImageImport fallo (0x%08x)", (unsigned)rc);
        return -1;
    }
    if ((rc = sysSpuThreadGroupCreate(&group_id, 1, 100, &group_attr)) != 0) {
        LOGE("raw: sysSpuThreadGroupCreate fallo (0x%08x)", (unsigned)rc);
        sysSpuImageClose(&image);
        return -1;
    }
    if ((rc = sysSpuThreadInitialize(&thread_id, group_id, 0, &image, &thread_attr, &arg)) != 0) {
        LOGE("raw: sysSpuThreadInitialize fallo (0x%08x)", (unsigned)rc);
        sysSpuThreadGroupDestroy(group_id);
        sysSpuImageClose(&image);
        return -1;
    }
    sysSpuThreadGroupStart(group_id);
    sysSpuThreadGroupJoin(group_id, &cause, &status);
    sysSpuThreadGroupDestroy(group_id);
    sysSpuImageClose(&image);
    *wall_us = udplog_now_us() - t0;

    __asm__ volatile ("sync" ::: "memory");
    if (!raw_job.done) {
        LOGE("raw: el SPE no termino (cause=%u status=%u)", cause, status);
        return -1;
    }
    *spu_ticks = raw_job.ticks;
    return 0;
}

/* ---- spukit ---------------------------------------------------------------- */

static spukit_worker *workers[WORKERS];

static int run_kit(int nworkers, const float *m, uint64_t *wall_us, uint32_t *spu_ticks)
{
    uint32_t seq[WORKERS];
    const uint32_t per = NVERT / nworkers;
    int rc = 0;

    const uint64_t t0 = udplog_now_us();
    for (int w = 0; w < nworkers; w++) {
        xform_params_t p;
        const uint32_t first = w * per;
        params(&p, m, first, w == nworkers - 1 ? NVERT - first : per);
        seq[w] = spukit_post(workers[w], XFORM_KIND, &p, sizeof p);
    }
    *spu_ticks = 0;
    for (int w = 0; w < nworkers; w++) {
        const spukit_result *r = spukit_wait(workers[w], seq[w]);
        if (r->error != 0) {
            LOGE("kit: trabajador %d devolvio error %d", w, (int)r->error);
            rc = -1;
        }
        if (r->ticks > *spu_ticks)
            *spu_ticks = r->ticks;          /* el más lento manda */
    }
    *wall_us = udplog_now_us() - t0;
    return rc;
}

/* ---- una pasada de los tres modos ----------------------------------------- */

typedef struct mode {
    const char *name;
    int         workers;                    /* 0 = hilo directo */
} xmode_t;

static const xmode_t modes[] = { { "raw", 0 }, { "kit x1", 1 }, { "kit x4", WORKERS } };

/* exact = 1: decide OK/FAIL; exact = 0: solo informa diferencias. */
static int run_pass(const char *label, const float *m, int exact)
{
    int rc = 0;

    const uint64_t tp = udplog_now_us();
    ppu_xform(m);
    const uint64_t ppu_us = udplog_now_us() - tp;
    LOGI("%s: referencia en el PPU %llu us (%.1f Mvert/s)", label, (unsigned long long)ppu_us,
         ppu_us ? NVERT / (double)ppu_us : 0.0);

    for (size_t k = 0; k < sizeof modes / sizeof modes[0]; k++) {
        const xmode_t *md = &modes[k];
        uint64_t best_us = ~0ull;
        uint32_t best_ticks = 0, worst_bad = 0;
        double max_units = 0.0;

        for (int run = 0; run < RUNS; run++) {
            uint64_t us = 0;
            uint32_t ticks = 0;
            double units = 0.0;
            memset(dst, 0xFF, NVERT * sizeof(vert_t));      /* NaN: nada viejo pasa la comparación */
            __asm__ volatile ("sync" ::: "memory");
            int r = md->workers ? run_kit(md->workers, m, &us, &ticks) : run_raw(m, &us, &ticks);
            __asm__ volatile ("sync" ::: "memory");
            if (r != 0) {
                rc = -1;
                break;
            }
            uint32_t bad = compare(m, &units);
            if (bad > worst_bad)
                worst_bad = bad;
            if (!(units <= max_units))
                max_units = units;
            if (us < best_us) {
                best_us = us;
                best_ticks = ticks;
            }
        }
        if (rc != 0)
            break;
        LOGI("%s %-6s: mejor de %d: %llu us total, %.0f us en SPU (%.1f Mvert/s total)",
             label, md->name, RUNS, (unsigned long long)best_us, tb_to_us(best_ticks),
             best_us ? NVERT / (double)best_us : 0.0);
        if (exact && worst_bad) {
            LOGE("%s %s: %u componentes distintos del PPU (error max %.1f u)", label, md->name, worst_bad, max_units);
            rc = -1;
        } else if (exact) {
            LOGI("%s %s: bit a bit igual al PPU", label, md->name);
        } else {
            LOGI("%s %s: %u de %u componentes difieren del PPU; error max %.2f u (u = 2^-24 de la escala)",
                 label, md->name, worst_bad, NVERT * 4, max_units);
        }
    }
    return rc;
}

static int test_spu(const ps3_env_t *env)
{
    (void)env;
    int rc = 0, started = 0;

    src = memalign(128, NVERT * sizeof(vert_t));
    dst = memalign(128, NVERT * sizeof(vert_t));
    ref = memalign(128, NVERT * sizeof(vert_t));
    if (!src || !dst || !ref) {
        LOGE("sin memoria para %u vertices", NVERT);
        return -1;
    }
    LOGI("%u vertices (%u KB por arreglo), trozos de %u por DMA con doble bufer, %d ejecuciones por modo",
         NVERT, (unsigned)(NVERT * sizeof(vert_t) / 1024), XFORM_CHUNK, RUNS);

    if (spukit_init(6) != SPUKIT_OK) {
        LOGE("spukit_init fallo");
        return -1;
    }
    for (; started < WORKERS; started++) {
        workers[started] = spukit_worker_start(spu_xform_kit_bin, "xform", 0);
        if (!workers[started]) {
            LOGE("spukit_worker_start(%d) fallo (%d)", started, spukit_last_error());
            rc = -1;
            break;
        }
    }

    if (rc == 0) {
        fill_exact();
        rc = run_pass("exactos", m_exact, 1);
    }
    if (rc == 0) {
        fill_any();
        run_pass("cualquiera", m_any, 0);
    }

    for (int w = 0; w < started; w++) {
        spukit_stats st;
        spukit_get_stats(workers[w], &st);
        LOGI("trabajador %d: %u trabajos, %u timbres, %u despertares, %llu us esperando",
             w, st.posted, st.doorbells, st.wakeups, (unsigned long long)st.wait_us);
        if (spukit_worker_stop(workers[w]) != 0) {
            LOGE("trabajador %d: spukit_worker_stop fallo", w);
            rc = -1;
        }
    }
    free(src);
    free(dst);
    free(ref);
    return rc;
}

int main(void)
{
    return phase0_main("spu", test_spu);
}
