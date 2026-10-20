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
 * DOL de prueba "hola": arranque, ganchos de salida y el formateador.
 * Todo lo que imprime tiene que salir igual que el mismo programa compilado
 * para la PC (tests/dol/esperado/hola.txt).
 */
#include "rt.h"

/* Una función con va_list propia: el gancho de vprintf lee la va_list. */
static void report_v(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report_v(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static const char *const words[] = { "uno", "dos", "tres" };

int main(void)
{
    OSReport("hola desde gc-ps3\n");
    OSReport("enteros: %d %i %u %x %X %o %c%c\n", -42, 7, 4000000000u, 0xBEEFu, 0xCAFEu, 8u, 'o', 'k');
    OSReport("anchos: [%5d] [%-5d] [%05d] [%+d] [% d] [%#x] [%#o]\n", 42, 42, 42, 42, 42, 255u, 8u);
    OSReport("cadenas: [%s] [%8s] [%-8s] [%.2s] [%s]\n", words[0], words[1], words[2], "abcdef", "");
    OSReport("cortos: %hd %hu %hhd %hhu\n", 70000, 70000u, 300, 300u);
    OSReport("largos: %lld %llu %llx %d\n", -1234567890123ll, 18446744073709551615ull, 0x123456789ABCDEFull, 5);
    OSReport("flotantes: %f %.3f %e %g %G %10.4f %-10.2e|\n", 3.14159265358979, 2.0 / 3.0, 123456.789,
             0.0001234, 1e20, -1.5, 6.02e23);
    OSReport("simple: %f %g\n", (double)1.1f, (double)(1.0f / 3.0f));
    OSReport("asteriscos: [%*d] [%-*d] [%.*f]\n", 6, 12, 6, 12, 2, 2.71828);
    OSReport("por ciento: 100%%\n");

    /* Más argumentos que registros: los que sobran van por la pila */
    OSReport("pila: %d %d %d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    OSReport("pila double: %g %g %g %g %g %g %g %g %g %g\n", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0,
             10.0);
    OSReport("mezcla: %d %lld %d %lld %g %d %lld %g\n", 1, 2ll, 3, 4ll, 5.5, 6, 7ll, 8.5);
    OSReport("pares al final: %d %d %d %d %d %d %lld %d\n", 1, 2, 3, 4, 5, 6, 77ll, 8);

    report_v("va_list: %s %d %lld %.2f %d %d %d %d %d %d %d\n", "ok", 1, 2ll, 3.25, 4, 5, 6, 7, 8, 9, 10);
    puts("puts: una linea");
    OSReport("sin salto al final");
    OSReport(" (sigue la linea)\n");
    return 0;
}
