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
 * Compara gx_exp2 (src/gx/exp2.c) con pow(2.0, x) de la libm para todos los
 * floats x en [-8, 0] (unos 1090 millones) y NaN. No es parte de las pruebas:
 * se corre a mano cuando cambia exp2.c o la glibc de la referencia.
 *
 *   gcc -std=c11 -O2 -ffp-contract=off -Isrc tools/exp2/comprobar.c \
 *       src/gx/exp2.c -lm -lpthread -o build/exp2-comprobar
 *   build/exp2-comprobar
 *
 * Cuenta las diferencias en tres niveles: el double de pow, el float que
 * guarda Tev.cpp (fog = pow(...) para los modos 6 y 7, fog = 1 - pow(...)
 * para los 4 y 5) y fogInt = (u32)(fog * 256), que es lo único que se usa.
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gx/exp2.h"
#include "gx/x86.h"

#define ULTIMO 0x41000000u /* 8.0f */
#define BLOQUE (1u << 20)
#define MUESTRAS 4

/* Por puntero: que el compilador no cambie pow(2, x) por exp2(x) */
static double (*volatile powp)(double, double) = pow;

enum { DBL, F67, F45, I67, I45, NCLASES };
static const char *const nombre[NCLASES] = {
    "double de pow", "float, modos 6 y 7", "float, modos 4 y 5", "fogInt, modos 6 y 7",
    "fogInt, modos 4 y 5",
};

typedef struct {
    unsigned id, hilos;
    uint64_t n[NCLASES];
    uint32_t muestra[NCLASES][MUESTRAS];
} trabajo_t;

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint64_t dbits(double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static void anota(trabajo_t *w, int c, uint32_t u)
{
    if (w->n[c] < MUESTRAS)
        w->muestra[c][w->n[c]] = u;
    w->n[c]++;
}

static void *corre(void *arg)
{
    trabajo_t *w = arg;
    for (uint64_t b = (uint64_t)w->id * BLOQUE; b <= ULTIMO; b += (uint64_t)w->hilos * BLOQUE) {
        const uint64_t fin = b + BLOQUE - 1 < ULTIMO ? b + BLOQUE - 1 : ULTIMO;
        for (uint64_t i = b; i <= fin; i++) {
            const uint32_t u = (uint32_t)i | 0x80000000u;
            float x;
            memcpy(&x, &u, 4);
            const double p = powp(2.0, (double)x);
            const double q = gx_exp2(x);
            if (dbits(p) == dbits(q))
                continue;
            anota(w, DBL, u);
            const float p67 = (float)p, q67 = (float)q;
            const float p45 = (float)(1.0 - p), q45 = (float)(1.0 - q);
            if (fbits(p67) != fbits(q67))
                anota(w, F67, u);
            if (fbits(p45) != fbits(q45))
                anota(w, F45, u);
            if (gx_x86_f2u(p67 * 256) != gx_x86_f2u(q67 * 256))
                anota(w, I67, u);
            if (gx_x86_f2u(p45 * 256) != gx_x86_f2u(q45 * 256))
                anota(w, I45, u);
        }
    }
    return NULL;
}

int main(void)
{
    const float nan = NAN;
    if (gx_exp2(nan) == gx_exp2(nan) || gx_exp2(-nan) == gx_exp2(-nan)) {
        printf("NaN: gx_exp2 no devuelve NaN\n");
        return 1;
    }

    long nh = sysconf(_SC_NPROCESSORS_ONLN);
    if (nh < 1)
        nh = 1;
    if (nh > 64)
        nh = 64;
    static trabajo_t w[64];
    pthread_t th[64];
    for (long i = 0; i < nh; i++) {
        w[i].id = (unsigned)i;
        w[i].hilos = (unsigned)nh;
        pthread_create(&th[i], NULL, corre, &w[i]);
    }
    uint64_t total[NCLASES] = { 0 };
    for (long i = 0; i < nh; i++) {
        pthread_join(th[i], NULL);
        for (int c = 0; c < NCLASES; c++)
            total[c] += w[i].n[c];
    }

    printf("%u floats en [-8, 0], %ld hilos\n", ULTIMO + 1, nh);
    int mal = 0;
    for (int c = 0; c < NCLASES; c++) {
        printf("  %-20s %llu diferencias", nombre[c], (unsigned long long)total[c]);
        int k = 0;
        for (long i = 0; i < nh && k < MUESTRAS; i++)
            for (uint64_t s = 0; s < w[i].n[c] && s < MUESTRAS && k < MUESTRAS; s++, k++) {
                float x;
                memcpy(&x, &w[i].muestra[c][s], 4);
                const double p = powp(2.0, (double)x), q = gx_exp2(x);
                printf("%s x=%a pow=%a propio=%a", k ? "," : ":", (double)x, p, q);
            }
        printf("\n");
        if (c != DBL && total[c])
            mal = 1;
    }
    printf(mal ? "DISTINTO\n" : "IGUAL (en float y fogInt)\n");
    return mal;
}
