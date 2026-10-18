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
#include <string.h>

#include "core/logline.h"
#include "test.h"

/* La misma línea de referencia la usa tools/logrecv.py --selftest. */
#define FIXTURE "GCPS3 0000beef 7 1500 I hello | hola mundo"

static void test_format_fixture(void)
{
    char buf[LOGLINE_MAX];
    int n = logline_format(buf, sizeof buf, 0xbeef, 7, 1500, 'I', "hello", "hola mundo");
    CHECK(n == (int)strlen(FIXTURE));
    CHECK(strcmp(buf, FIXTURE) == 0);
}

static void test_parse_roundtrip(void)
{
    char buf[LOGLINE_MAX];
    logline_t l;
    logline_format(buf, sizeof buf, 0xdeadbeef, 4294967295u, 18446744073709551615ull,
                   'R', "jit_nobits-2", "RESULT: OK | con barra");
    CHECK(logline_parse(buf, &l) == 0);
    CHECK_EQ_U64(l.session, 0xdeadbeef);
    CHECK_EQ_U64(l.seq, 4294967295u);
    CHECK_EQ_U64(l.t_us, 18446744073709551615ull);
    CHECK(l.level == 'R');
    CHECK(strcmp(l.test, "jit_nobits-2") == 0);
    CHECK(strcmp(l.msg, "RESULT: OK | con barra") == 0);
}

static void test_truncate_long_message(void)
{
    char msg[2000], buf[LOGLINE_MAX];
    logline_t l;
    memset(msg, 'x', sizeof msg - 1);
    msg[sizeof msg - 1] = '\0';
    int n = logline_format(buf, sizeof buf, 1, 2, 3, 'W', "udplog", msg);
    CHECK(n == LOGLINE_MAX - 1);
    CHECK(logline_parse(buf, &l) == 0);
    CHECK(strlen(l.msg) > 400);
}

static void test_newlines_flattened(void)
{
    char buf[LOGLINE_MAX];
    logline_format(buf, sizeof buf, 1, 0, 0, 'E', "iso", "linea 1\nlinea 2\r");
    CHECK(strchr(buf, '\n') == NULL && strchr(buf, '\r') == NULL);
}

static void test_reject_invalid(void)
{
    char buf[LOGLINE_MAX];
    logline_t l;
    CHECK(logline_format(buf, sizeof buf, 1, 0, 0, 'X', "hello", "m") == -1);
    CHECK(logline_format(buf, sizeof buf, 1, 0, 0, 'I', "Hola Mundo", "m") == -1);
    CHECK(logline_format(buf, sizeof buf, 1, 0, 0, 'I', "", "m") == -1);
    CHECK(logline_parse("hola", &l) == -1);
    CHECK(logline_parse("GCPS3 beef 7 1500 I hello | m", &l) == -1);        /* sesión corta */
    CHECK(logline_parse("GCPS3 0000beef 7 1500 Q hello | m", &l) == -1);    /* nivel */
    CHECK(logline_parse("GCPS3 0000beef 7 1500 I hello m", &l) == -1);      /* sin separador */
    CHECK(logline_parse("GCPS3 0000beef 99999999999 1500 I hello | m", &l) == -1); /* seq > 32 bits */
}

const test_case_t logline_tests[] = {
    { "logline: línea de referencia",   test_format_fixture },
    { "logline: ida y vuelta",          test_parse_roundtrip },
    { "logline: mensaje largo truncado", test_truncate_long_message },
    { "logline: sin saltos de línea",   test_newlines_flattened },
    { "logline: rechaza líneas malas",  test_reject_invalid },
    { NULL, NULL },
};
