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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/file.h>

#include "platform/ps3/files.h"

int ps3_read_file(const char *path, uint8_t **data, size_t *len, size_t max)
{
    sysFSStat st;
    s32 fd;
    *data = NULL;
    *len = 0;
    if (sysLv2FsStat(path, &st) != 0 || st.st_size > max)
        return -1;
    const size_t size = (size_t)st.st_size;
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf)
        return -1;
    if (sysLv2FsOpen(path, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0) {
        free(buf);
        return -1;
    }
    size_t done = 0;
    while (done < size) {
        u64 n = 0;
        if (sysLv2FsRead(fd, buf + done, size - done, &n) != 0 || n == 0)
            break;
        done += (size_t)n;
    }
    sysLv2FsClose(fd);
    if (done != size) {
        free(buf);
        return -1;
    }
    *data = buf;
    *len = size;
    return 0;
}

static int ends_with_dol(const char *name, size_t n)
{
    if (n < 5)
        return 0;
    const char *e = name + n - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'd' && (e[2] | 0x20) == 'o' && (e[3] | 0x20) == 'l';
}

int ps3_list_dols(const char *dir, int bundled, ps3_dol_t *list, int *count, int max)
{
    s32 fd;
    int added = 0;
    if (sysLv2FsOpenDir(dir, &fd) != 0)
        return 0;
    for (;;) {
        sysFSDirent e;
        u64 n = 0;
        if (sysLv2FsReadDir(fd, &e, &n) != 0 || n == 0)
            break;
        const size_t len = strnlen(e.d_name, sizeof e.d_name);
        if (!ends_with_dol(e.d_name, len) || len >= PS3_NAME_MAX || *count >= max)
            continue;
        ps3_dol_t *d = &list[*count];
        snprintf(d->name, sizeof d->name, "%.*s", (int)len, e.d_name);
        snprintf(d->path, sizeof d->path, "%s/%s", dir, d->name);
        d->bundled = bundled;
        /* Inserción ordenada por nombre */
        int k = *count;
        while (k > 0 && strcmp(list[k - 1].name, d->name) > 0)
            k--;
        if (k != *count) {
            const ps3_dol_t tmp = *d;
            memmove(&list[k + 1], &list[k], (size_t)(*count - k) * sizeof *list);
            list[k] = tmp;
        }
        (*count)++;
        added++;
    }
    sysLv2FsCloseDir(fd);
    return added;
}

void ps3_map_path(const char *dol_path, char *out, size_t size)
{
    snprintf(out, size, "%s", dol_path);
    const size_t n = strlen(out);
    if (n >= 4 && out[n - 4] == '.')
        memcpy(out + n - 3, "map", 3);
}
