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
 * Prueba "iso" (etapa C): lectura de un archivo del tamaño de un disco de
 * GameCube desde el HDD (y desde USB si está), verificando cada byte con el
 * patrón de src/core/pattern.h (lo genera tools/mkiso.py).
 *
 *   secuencial  todo el archivo en lecturas de 1 MB
 *   aleatoria   1000 lecturas de 32 KB en sectores de 2 KB al azar
 *
 * FAIL si falta el archivo del HDD o si un solo byte no coincide. La velocidad
 * se informa (en RPCS3 es la del disco de la PC y no cuenta); en consola el
 * criterio es quedar muy por encima de los ~3 MB/s del lector de la GameCube.
 */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/file.h>

#include "common/harness.h"
#include "core/pattern.h"

#define SEQ_READ      (1u << 20)
#define RAND_READ     (32u << 10)
#define RAND_COUNT    1000u
#define SECTOR        2048u
#define MAX_REPORTS   5

typedef struct target {
    const char *path;
    int         required;
} target_t;

static const target_t targets[] = {
    { "/dev_hdd0/tmp/gcps3/patron.bin",  1 },
    { "/dev_usb000/gcps3/patron.bin",    0 },
};

static uint8_t *buf;
static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t xorshift64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static double mb_per_s(uint64_t bytes, uint64_t us)
{
    return us ? (double)bytes / (double)us : 0.0;     /* bytes/us = MB/s (10^6) */
}

/* Lee exactamente len bytes desde la posición actual; 0 si pudo. */
static int read_all(s32 fd, uint8_t *dst, uint32_t len, uint64_t *read_us)
{
    uint32_t done = 0;
    const uint64_t t0 = udplog_now_us();
    while (done < len) {
        u64 n = 0;
        s32 rc = sysLv2FsRead(fd, dst + done, len - done, &n);
        if (rc != 0 || n == 0) {
            LOGE("lectura fallo (0x%08x) tras %u de %u bytes", (unsigned)rc, done, len);
            return -1;
        }
        done += (uint32_t)n;
    }
    *read_us += udplog_now_us() - t0;
    return 0;
}

static int check_block(uint64_t offset, uint32_t len, uint32_t *reports)
{
    long at = pattern_check(buf, offset, len);
    if (at < 0)
        return 0;
    if ((*reports)++ < MAX_REPORTS) {
        const uint8_t *p = buf + at;
        LOGE("byte %llu: leido %02x%02x%02x%02x, esperado %08x",
             (unsigned long long)(offset + (uint64_t)at), p[0], p[1], p[2], p[3],
             (unsigned)(uint32_t)(offset + (uint64_t)at));
    }
    return -1;
}

static int sequential(s32 fd, const char *name, uint64_t size)
{
    uint64_t read_us = 0, verify_us = 0, off = 0;
    uint32_t bad = 0, reports = 0;
    const uint64_t t0 = udplog_now_us();

    if (sysLv2FsLSeek64(fd, 0, 0, &off) != 0) {
        LOGE("%s: lseek fallo", name);
        return -1;
    }
    for (off = 0; off < size; off += SEQ_READ) {
        uint32_t len = (uint32_t)(size - off < SEQ_READ ? size - off : SEQ_READ);
        len &= ~3u;
        if (read_all(fd, buf, len, &read_us) != 0)
            return -1;
        const uint64_t tv = udplog_now_us();
        if (check_block(off, len, &reports) != 0)
            bad++;
        verify_us += udplog_now_us() - tv;
        if ((off + SEQ_READ) % (256u << 20) == 0)
            LOGI("%s: %llu MB leidos...", name, (unsigned long long)((off + SEQ_READ) >> 20));
    }
    const uint64_t total_us = udplog_now_us() - t0;
    LOGI("%s secuencial: %llu MB en %.2f s; lectura %.1f MB/s, verificacion %.1f MB/s, total %.1f MB/s",
         name, (unsigned long long)(size >> 20), total_us / 1e6, mb_per_s(size, read_us),
         mb_per_s(size, verify_us), mb_per_s(size, total_us));
    if (bad) {
        LOGE("%s secuencial: %u bloques de 1 MB con datos distintos", name, bad);
        return -1;
    }
    LOGI("%s secuencial: todos los bytes coinciden", name);
    return 0;
}

static int random_reads(s32 fd, const char *name, uint64_t size)
{
    const uint64_t sectors = (size - RAND_READ) / SECTOR;
    uint64_t read_us = 0, min_us = ~0ull, max_us = 0;
    uint32_t bad = 0, reports = 0;

    for (uint32_t i = 0; i < RAND_COUNT; i++) {
        const uint64_t off = (xorshift64() % (sectors + 1)) * SECTOR;
        uint64_t pos, one = 0;
        const uint64_t t0 = udplog_now_us();
        if (sysLv2FsLSeek64(fd, off, 0, &pos) != 0 || pos != off) {
            LOGE("%s: lseek a %llu fallo", name, (unsigned long long)off);
            return -1;
        }
        if (read_all(fd, buf, RAND_READ, &one) != 0)
            return -1;
        const uint64_t dt = udplog_now_us() - t0;       /* lseek + lectura */
        read_us += dt;
        if (dt < min_us) min_us = dt;
        if (dt > max_us) max_us = dt;
        if (check_block(off, RAND_READ, &reports) != 0)
            bad++;
    }
    LOGI("%s aleatoria: %u x 32 KB; por lectura min %llu us, media %llu us, max %llu us; %.1f MB/s",
         name, RAND_COUNT, (unsigned long long)min_us, (unsigned long long)(read_us / RAND_COUNT),
         (unsigned long long)max_us, mb_per_s((uint64_t)RAND_COUNT * RAND_READ, read_us));
    if (bad) {
        LOGE("%s aleatoria: %u lecturas con datos distintos", name, bad);
        return -1;
    }
    LOGI("%s aleatoria: todos los bytes coinciden", name);
    return 0;
}

static int test_target(const target_t *t)
{
    sysFSStat st;
    s32 fd = -1;
    const char *name = t->required ? "hdd" : "usb";

    if (sysLv2FsStat(t->path, &st) != 0) {
        if (t->required) {
            LOGE("no existe %s: generarlo en la PC con python3 tools/mkiso.py patron.bin y copiarlo ahi",
                 t->path);
            return -1;
        }
        LOGW("no existe %s: se omite la prueba desde USB", t->path);
        return 0;
    }
    const uint64_t size = st.st_size & ~3ull;
    LOGI("%s: %s, %llu bytes", name, t->path, (unsigned long long)st.st_size);
    if (st.st_size != PATTERN_GC_DISC_SIZE)
        LOGW("%s: no mide %llu bytes como un disco de GameCube", name, PATTERN_GC_DISC_SIZE);
    if (size < 2 * RAND_READ) {
        LOGE("%s: archivo demasiado chico", name);
        return -1;
    }
    if (sysLv2FsOpen(t->path, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0) {
        LOGE("%s: no se pudo abrir", name);
        return -1;
    }
    int rc = sequential(fd, name, size);
    if (rc == 0)
        rc = random_reads(fd, name, size);
    sysLv2FsClose(fd);
    return rc;
}

static int test_iso(const ps3_env_t *env)
{
    (void)env;
    int rc = 0;

    buf = memalign(128, SEQ_READ);
    if (!buf) {
        LOGE("sin memoria para el bufer de 1 MB");
        return -1;
    }
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++)
        if (test_target(&targets[i]) != 0)
            rc = -1;
    free(buf);
    return rc;
}

int main(void)
{
    return phase0_main("iso", test_iso);
}
