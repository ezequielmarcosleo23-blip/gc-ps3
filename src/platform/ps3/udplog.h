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
 * Log por UDP hacia la PC (y copia en la TTY).
 *
 * Destinos: si existe /dev_hdd0/tmp/gcps3/log.cfg con una IP ("192.168.1.10"
 * o "192.168.1.10:18194"), solo esa; si no, broadcast 255.255.255.255 y
 * 127.0.0.1 (este último sirve en RPCS3). El receptor descarta duplicados por
 * número de secuencia. No es seguro entre hilos todavía.
 */
#ifndef GCPS3_PLATFORM_PS3_UDPLOG_H
#define GCPS3_PLATFORM_PS3_UDPLOG_H

#include <stdint.h>

#define UDPLOG_CFG_PATH "/dev_hdd0/tmp/gcps3/log.cfg"

/* Inicia la red y el socket. Sin red, el log sigue saliendo por la TTY. */
int  udplog_init(const char *test_name);
void udplog_printf(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void udplog_result(int ok);
void udplog_shutdown(void);

uint32_t udplog_sent(void);          /* líneas enviadas */
uint32_t udplog_send_errors(void);   /* sendto fallidos (un destino puede fallar y otro no) */
uint64_t udplog_now_us(void);        /* µs desde udplog_init (timebase del PPU) */
const char *udplog_targets(void);    /* destinos en texto, para el log */

#endif
