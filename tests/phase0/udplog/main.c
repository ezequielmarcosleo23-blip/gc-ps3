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
 * Prueba "udplog" (etapa C): cuánto aguanta el log por UDP.
 *
 * Cuatro ráfagas de 10 000 líneas de detalle (nivel D, solo UDP):
 *   r1     sin pausa (lo más rápido que da el PPU)
 *   r2     1000 líneas/s
 *   r3     100 líneas/s (dura 100 s)
 *   hilos  4 hilos del PPU a la vez, 2500 líneas cada uno, sin pausa
 * Cada línea dice "<ráfaga> <i>/<total>"; tools/udplog_stats.py cuenta en la PC
 * cuántas llegaron de cada ráfaga. La consola no puede saber qué se perdió en
 * la red: aquí solo se mide el costo de cada línea en el PPU y los envíos
 * fallidos. FAIL si un hilo no arranca o si fallan envíos a ritmo normal.
 */
#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <lv2/thread.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "common/harness.h"

#define LINES        10000u
#define THREADS      4u
#define PAUSE_US     (2u * 1000u * 1000u)   /* entre ráfagas, para que el receptor se ponga al día */

typedef struct burst {
    const char *name;
    uint32_t    rate;                      /* líneas/s; 0 = sin pausa */
} burst_t;

static const burst_t bursts[] = {
    { "r1", 0 },
    { "r2", 1000 },
    { "r3", 100 },
};

/* Espera hasta t_us (desde udplog_init). sysUsleep tiene grano grueso:
 * duerme lo largo y deja lo último a un sysThreadYield. */
static void wait_until(uint64_t t_us)
{
    for (;;) {
        uint64_t now = udplog_now_us();
        if (now >= t_us)
            return;
        uint64_t left = t_us - now;
        if (left > 300)
            sysUsleep((u32)(left - 200));
        else
            sysThreadYield();
    }
}

static int run_burst(const burst_t *b)
{
    const uint32_t err0 = udplog_send_errors();

    if (b->rate)
        LOGI("rafaga %s: %u lineas a %u/s", b->name, LINES, b->rate);
    else
        LOGI("rafaga %s: %u lineas sin pausa", b->name, LINES);

    const uint64_t t0 = udplog_now_us();
    for (uint32_t i = 0; i < LINES; i++) {
        if (b->rate)
            wait_until(t0 + (uint64_t)i * 1000000u / b->rate);
        LOGD("%s %u/%u", b->name, i, LINES);
    }
    const uint64_t dt = udplog_now_us() - t0;
    const uint32_t errors = udplog_send_errors() - err0;

    LOGI("rafaga %s: %u lineas en %llu us (%.2f us/linea, %.0f lineas/s), %u envios fallidos",
         b->name, LINES, (unsigned long long)dt, (double)dt / LINES,
         dt ? LINES * 1e6 / (double)dt : 0.0, errors);
    if (errors && b->rate) {
        LOGE("rafaga %s: hubo envios fallidos a ritmo normal", b->name);
        return -1;
    }
    if (errors)
        LOGW("rafaga %s: envios fallidos sin pausa (el PPU va mas rapido que la red)", b->name);
    return 0;
}

static void writer(void *arg)
{
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    const uint32_t n = LINES / THREADS;
    for (uint32_t i = 0; i < n; i++)
        LOGD("h%u %u/%u", id, i, n);
    sysThreadExit(0);
}

static int run_threads(void)
{
    sys_ppu_thread_t tid[THREADS];
    char name[THREADS][16];
    uint32_t started = 0;
    const uint32_t err0 = udplog_send_errors();

    LOGI("rafaga hilos: %u hilos x %u lineas a la vez", THREADS, LINES / THREADS);
    const uint64_t t0 = udplog_now_us();
    for (uint32_t t = 0; t < THREADS; t++) {
        snprintf(name[t], sizeof name[t], "udplog_h%u", t);
        s32 rc = sysThreadCreate(&tid[t], writer, (void *)(uintptr_t)t, 1000, 64 * 1024,
                                 THREAD_JOINABLE, name[t]);
        if (rc != 0) {
            LOGE("hilos: sysThreadCreate(%u) fallo (0x%08x)", t, (unsigned)rc);
            break;
        }
        started++;
    }
    for (uint32_t t = 0; t < started; t++) {
        u64 ret;
        sysThreadJoin(tid[t], &ret);
    }
    const uint64_t dt = udplog_now_us() - t0;
    LOGI("rafaga hilos: %u lineas en %llu us (%.2f us/linea), %u envios fallidos",
         started * (LINES / THREADS), (unsigned long long)dt,
         started ? (double)dt / (started * (LINES / THREADS)) : 0.0,
         udplog_send_errors() - err0);
    return started == THREADS ? 0 : -1;
}

static int test_udplog(const ps3_env_t *env)
{
    (void)env;
    int rc = 0;

    LOGI("analisis en la PC: python3 tools/udplog_stats.py logs/<este log>");
    for (size_t k = 0; k < sizeof bursts / sizeof bursts[0]; k++) {
        if (run_burst(&bursts[k]) != 0)
            rc = -1;
        sysUsleep(PAUSE_US);
    }
    if (run_threads() != 0)
        rc = -1;
    sysUsleep(PAUSE_US);
    return rc;
}

int main(void)
{
    return phase0_main("udplog", test_udplog);
}
