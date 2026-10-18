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
#include <string.h>

#include "core/logline.h"

int logline_valid_test_name(const char *test)
{
    size_t n = 0;

    if (!test)
        return 0;
    for (; test[n]; n++) {
        char c = test[n];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= LOGLINE_TEST_MAX)
            return 0;
    }
    return n > 0;
}

int logline_valid_level(char level)
{
    return level == 'I' || level == 'W' || level == 'E' || level == 'R';
}

int logline_format(char *buf, size_t size, uint32_t session, uint32_t seq,
                   uint64_t t_us, char level, const char *test, const char *msg)
{
    if (!buf || size < 64 || !logline_valid_level(level) || !logline_valid_test_name(test))
        return -1;
    if (!msg)
        msg = "";

    int head = snprintf(buf, size, "%s %08x %lu %llu %c %s | ", LOGLINE_MAGIC,
                        (unsigned)session, (unsigned long)seq,
                        (unsigned long long)t_us, level, test);
    if (head < 0 || (size_t)head >= size)
        return -1;

    /* El mensaje va en una sola línea: los saltos se cambian por espacios. */
    size_t pos = (size_t)head;
    for (const char *p = msg; *p && pos + 1 < size; p++)
        buf[pos++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    buf[pos] = '\0';
    return (int)pos;
}

static const char *parse_u64(const char *p, int base, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;
    int digits = 0;

    for (;; p++, digits++) {
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else
            break;
        if (v > (max - (uint64_t)d) / (uint64_t)base)
            return NULL;                       /* desbordamiento */
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (digits == 0 || *p != ' ')
        return NULL;
    *out = v;
    return p + 1;
}

int logline_parse(const char *line, logline_t *out)
{
    uint64_t v;
    const char *p = line;

    if (!line || !out || strncmp(p, LOGLINE_MAGIC " ", sizeof LOGLINE_MAGIC) != 0)
        return -1;
    p += sizeof LOGLINE_MAGIC;

    if (strspn(p, "0123456789abcdef") != 8 || !(p = parse_u64(p, 16, UINT32_MAX, &v)))
        return -1;
    out->session = (uint32_t)v;
    if (!(p = parse_u64(p, 10, UINT32_MAX, &v)))
        return -1;
    out->seq = (uint32_t)v;
    if (!(p = parse_u64(p, 10, UINT64_MAX, &v)))
        return -1;
    out->t_us = v;

    if (!logline_valid_level(p[0]) || p[1] != ' ')
        return -1;
    out->level = p[0];
    p += 2;

    const char *sep = strstr(p, " | ");
    size_t n = sep ? (size_t)(sep - p) : 0;
    if (!sep || n == 0 || n > LOGLINE_TEST_MAX)
        return -1;
    memcpy(out->test, p, n);
    out->test[n] = '\0';
    if (!logline_valid_test_name(out->test))
        return -1;

    out->msg = sep + 3;
    return 0;
}
