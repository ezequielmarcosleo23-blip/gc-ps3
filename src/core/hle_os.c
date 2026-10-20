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
 * Ganchos de depuración: lo que el programa imprime con OSReport, printf,
 * vprintf, puts o __write_console va al log (o al runner), y exit / _exit
 * paran la CPU. La lista y las firmas son las de Dolphin
 * (Source/Core/Core/HLE/HLE.cpp y HLE_OS.cpp); todos son "al inicio": la
 * función sigue corriendo después, como en el hardware.
 */
#include "core/hle.h"
#include "core/log.h"
#include "core/system.h"
#include "core/vaformat.h"

#define TEXT_MAX 4096

static char text[TEXT_MAX];

/* printf(fmt, ...) y parecidas: formato en r3, argumentos desde r4. */
static void hle_printf(gc_system_t *s)
{
    gc_va_t va;
    gc_va_from_regs(&va, &s->mem, &s->cpu, 4);
    const size_t n = gc_vaformat(&s->mem, s->cpu.gpr[3], &va, text, sizeof text);
    gc_hle_output(s, text, n);
}

/* vprintf(fmt, va_list) */
static void hle_vprintf(gc_system_t *s)
{
    gc_va_t va;
    gc_va_from_list(&va, &s->mem, &s->cpu, s->cpu.gpr[4]);
    const size_t n = gc_vaformat(&s->mem, s->cpu.gpr[3], &va, text, sizeof text);
    gc_hle_output(s, text, n);
}

/* puts(s): la cadena y un salto de línea */
static void hle_puts(gc_system_t *s)
{
    const size_t n = gc_read_cstr(&s->mem, s->cpu.gpr[3], text, sizeof text);
    gc_hle_output(s, text, n);
    gc_hle_output(s, "\n", 1);
}

/* __write_console(int fd, const void *buffer, const u32 *size), como en Dolphin
 * (lo usa el menú del sistema); con libogc se comprueba en la etapa D. */
static void hle_write_console(gc_system_t *s)
{
    const uint32_t size = gc_read32(&s->mem, s->cpu.gpr[5]);
    size_t n = gc_read_cstr(&s->mem, s->cpu.gpr[4], text, sizeof text);
    if (size && size < n)
        n = size;
    gc_hle_output(s, text, n);
}

/* exit(code) y _exit(code): el runner termina con ese código */
static void hle_exit(gc_system_t *s)
{
    gc_hle_flush(s);
    if (!s->hle.exited)
        GC_LOGI("hle: exit(%d) en 0x%08x", (int)s->cpu.gpr[3], (unsigned)s->cpu.pc);
    s->hle.exited = 1;
    s->hle.exit_code = s->cpu.gpr[3];
    s->cpu.stop = 1;
}

const gc_hle_known_t gc_hle_known[] = {
    { "OSReport", hle_printf },
    { "DEBUGPrint", hle_printf },
    { "printf", hle_printf },
    { "vprintf", hle_vprintf },
    { "puts", hle_puts },
    { "__write_console", hle_write_console },
    { "exit", hle_exit },
    { "_exit", hle_exit },
    { NULL, NULL },
};
