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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ppu-asm.h>
#include <ppu-types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/net.h>
#include <net/socket.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "core/logline.h"
#include "platform/ps3/udplog.h"

#define MAX_TARGETS 2

static struct {
    int                sock;
    int                net_up;
    int                ntargets;
    struct sockaddr_in targets[MAX_TARGETS];
    char               targets_txt[64];
    char               test[LOGLINE_TEST_MAX + 1];
    uint32_t           session;
    uint32_t           seq;
    uint32_t           sent;
    uint32_t           errors;
    uint64_t           tb0;
    uint64_t           tb_freq;
} lg = { .sock = -1 };

uint64_t udplog_now_us(void)
{
    if (!lg.tb_freq)
        return 0;
    uint64_t ticks = __gettime() - lg.tb0;
    /* Separado en segundos + resto para no desbordar en ejecuciones largas. */
    return (ticks / lg.tb_freq) * 1000000ull + (ticks % lg.tb_freq) * 1000000ull / lg.tb_freq;
}

static void add_target(uint32_t ip_host_order, uint16_t port)
{
    if (lg.ntargets >= MAX_TARGETS)
        return;
    struct sockaddr_in *a = &lg.targets[lg.ntargets++];
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = htonl(ip_host_order);
}

/* Lee "a.b.c.d" o "a.b.c.d:puerto" de log.cfg; 1 si encontró un destino válido. */
static int read_config(void)
{
    FILE *f = fopen(UDPLOG_CFG_PATH, "r");
    char line[64];
    unsigned a, b, c, d, port = LOGLINE_PORT;

    if (!f)
        return 0;
    int ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok)
        return 0;
    int n = sscanf(line, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port);
    if (n < 4 || a > 255 || b > 255 || c > 255 || d > 255 || port == 0 || port > 65535)
        return 0;
    add_target((a << 24) | (b << 16) | (c << 8) | d, (uint16_t)port);
    snprintf(lg.targets_txt, sizeof lg.targets_txt, "%u.%u.%u.%u:%u (log.cfg)", a, b, c, d, port);
    return 1;
}

int udplog_init(const char *test_name)
{
    lg.tb_freq = sysGetTimebaseFrequency();
    lg.tb0 = __gettime();
    lg.session = (uint32_t)(lg.tb0 ^ (lg.tb0 >> 32) ^ ((uint64_t)sysProcessGetPid() << 16));
    lg.seq = lg.sent = lg.errors = 0;
    lg.ntargets = 0;
    snprintf(lg.test, sizeof lg.test, "%s", logline_valid_test_name(test_name) ? test_name : "gc-ps3");

    if (netInitialize() < 0) {
        printf("udplog: netInitialize fallo; solo TTY\n");
        snprintf(lg.targets_txt, sizeof lg.targets_txt, "solo TTY");
        return -1;
    }
    lg.net_up = 1;

    lg.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (lg.sock < 0) {
        printf("udplog: socket fallo; solo TTY\n");
        snprintf(lg.targets_txt, sizeof lg.targets_txt, "solo TTY");
        return -1;
    }

    if (!read_config()) {
        int on = 1;
        setsockopt(lg.sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
        add_target(INADDR_BROADCAST, LOGLINE_PORT);
        add_target(INADDR_LOOPBACK, LOGLINE_PORT);
        snprintf(lg.targets_txt, sizeof lg.targets_txt,
                 "255.255.255.255 y 127.0.0.1, puerto %d", LOGLINE_PORT);
    }
    return 0;
}

static void send_line(char level, const char *msg)
{
    char line[LOGLINE_MAX];

    if (logline_format(line, sizeof line, lg.session, lg.seq++, udplog_now_us(),
                       level, lg.test, msg) < 0)
        return;
    if (lg.sock < 0)
        return;
    for (int i = 0; i < lg.ntargets; i++) {
        if (sendto(lg.sock, line, strlen(line), 0,
                   (const struct sockaddr *)&lg.targets[i], sizeof lg.targets[i]) < 0)
            lg.errors++;
    }
    lg.sent++;
}

void udplog_printf(char level, const char *fmt, ...)
{
    char msg[LOGLINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    printf("%s\n", msg);               /* copia en la TTY */
    send_line(level, msg);
}

void udplog_result(int ok)
{
    const char *msg = ok ? "RESULT: OK" : "RESULT: FAIL";
    printf("%s\n", msg);
    send_line('R', msg);
}

void udplog_shutdown(void)
{
    if (lg.sock >= 0) {
        sysUsleep(200 * 1000);         /* deja salir los últimos datagramas */
        close(lg.sock);
        lg.sock = -1;
    }
    if (lg.net_up) {
        netDeinitialize();
        lg.net_up = 0;
    }
}

uint32_t udplog_sent(void) { return lg.sent; }
uint32_t udplog_send_errors(void) { return lg.errors; }
const char *udplog_targets(void) { return lg.targets_txt; }
