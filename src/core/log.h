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
 * Log del núcleo portable. El núcleo no sabe dónde termina el mensaje: en la
 * PC va a stderr; en la PS3 la plataforma conecta el log UDP con
 * gc_log_set_sink(). Niveles como los del log UDP: 'I', 'W', 'E', 'D'.
 */
#ifndef GCPS3_CORE_LOG_H
#define GCPS3_CORE_LOG_H

typedef void (*gc_log_sink_fn)(char level, const char *msg);

/* NULL vuelve al destino por omisión (stderr). */
void gc_log_set_sink(gc_log_sink_fn fn);

void gc_logf(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define GC_LOGI(...) gc_logf('I', __VA_ARGS__)
#define GC_LOGW(...) gc_logf('W', __VA_ARGS__)
#define GC_LOGE(...) gc_logf('E', __VA_ARGS__)

#endif
