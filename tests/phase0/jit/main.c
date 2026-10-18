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
 * Prueba "jit" (etapa C, método 1 de 3): código generado en una cola NOBITS
 * del segmento de texto (nobits.ld), el camino que Salsa64 validó con HEN.
 *
 * Pasos, cada uno con su marca en disco:
 *   leer        toda la cola es legible y vale cero (el cargador la mapeó)
 *   ejecutar    escribe r3 = r3 * 3 + 7; blr al inicio, limpia cachés y la llama
 *   modificar   cambia el 7 por un 9, limpia y llama: si sale el valor viejo,
 *               el PPE ejecutó instrucciones viejas de la caché
 *   final       lo mismo en los últimos 4 KB de la cola
 *   limpieza    tiempo de dcbst/sync/icbi/isync sobre 64 KB
 *
 * Saltar a memoria no ejecutable mata el proceso sin aviso. Por eso antes de
 * cada paso se agrega "PROBANDO <paso>" a JIT_STATE (y "OK <paso>" después).
 * Si la ejecución anterior quedó en PROBANDO, esta informa en qué paso murió,
 * guarda el archivo como JIT_CRASH y termina con FAIL sin repetirlo; la
 * siguiente empieza de cero.
 *
 * Solo en consola: RPCS3 no ejecuta bien código generado. Los métodos 2 y 3
 * (syscall 905 y page_allocate de PS3MAPI) quedan para la consola con CFW.
 */
#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/file.h>

#include "common/harness.h"
#include "platform/ps3/paths.h"

#define NOBITS_MB    32u                       /* ±32 MB: alcance de un salto relativo */
#define NOBITS_BYTES (NOBITS_MB * 1024u * 1024u)
#define TAIL_BYTES   4096u
#define FLUSH_BYTES  (64u * 1024u)
#define FLUSH_REPS   100
#define CACHE_LINE   128u

#define DATA_DIR  GCPS3_DATA_DIR
#define JIT_STATE DATA_DIR "/jit_estado.txt"
#define JIT_CRASH DATA_DIR "/jit_crash.txt"

#define STR_(x) #x
#define STR(x)  STR_(x)

/* La cola: "ax" (ejecutable) y @nobits (sin bytes en el archivo). */
__asm__("    .section .gcps3_jit_nobits,\"ax\",@nobits\n"
        "    .balign 65536\n"
        "    .globl gcps3_jit_nobits\n"
        "gcps3_jit_nobits:\n"
        "    .space " STR(NOBITS_BYTES) "\n"
        "    .previous\n");
extern u32 gcps3_jit_nobits[];

/* ---- marca de crash en disco ------------------------------------------------ */

static void state_append(const char *text)
{
    s32 fd;
    u64 written;
    if (sysLv2FsOpen(JIT_STATE, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, NULL, 0) != 0)
        return;
    sysLv2FsWrite(fd, text, strlen(text), &written);
    sysLv2FsFsync(fd);
    sysLv2FsClose(fd);
}

static void step_begin(const char *step)
{
    char line[64];
    snprintf(line, sizeof line, "PROBANDO %s\n", step);
    state_append(line);
    LOGI("paso %s...", step);
}

static void step_ok(const char *step)
{
    char line[64];
    snprintf(line, sizeof line, "OK %s\n", step);
    state_append(line);
}

/* 1 si la ejecución anterior murió a mitad de un paso (y dice cuál). */
static int previous_crash(void)
{
    char buf[1024], last[64] = "";
    s32 fd;
    u64 n = 0;

    if (sysLv2FsOpen(JIT_STATE, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0)
        return 0;
    sysLv2FsRead(fd, buf, sizeof buf - 1, &n);
    sysLv2FsClose(fd);
    buf[n] = '\0';

    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
        snprintf(last, sizeof last, "%s", line);
    if (strncmp(last, "PROBANDO ", 9) != 0) {
        sysLv2FsUnlink(JIT_STATE);
        return 0;
    }
    LOGE("la ejecucion anterior se cayo en el paso '%s' (ver %s)", last + 9, JIT_CRASH);
    sysLv2FsUnlink(JIT_CRASH);
    sysLv2FsRename(JIT_STATE, JIT_CRASH);
    return 1;
}

/* ---- código generado ---------------------------------------------------------- */

#define PPC_MULLI_R3_R3(imm) (0x1C630000u | ((u32)(imm) & 0xFFFFu))
#define PPC_ADDI_R3_R3(imm)  (0x38630000u | ((u32)(imm) & 0xFFFFu))
#define PPC_BLR              0x4E800020u

static void emit(volatile u32 *code, int add)
{
    code[0] = PPC_MULLI_R3_R3(3);
    code[1] = PPC_ADDI_R3_R3(add);
    code[2] = PPC_BLR;
}

/* Secuencia de PowerPC Book II para código modificado. */
static void flush_code(const void *p, size_t n)
{
    uintptr_t start = (uintptr_t)p & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end = (uintptr_t)p + n;

    for (uintptr_t a = start; a < end; a += CACHE_LINE)
        __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
    for (uintptr_t a = start; a < end; a += CACHE_LINE)
        __asm__ volatile("icbi 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("isync" : : : "memory");
}

/* ABI ELFv1 de 64 bits (PSL1GHT): un puntero a función apunta a un
 * descriptor {entrada, TOC, entorno}; se reutiliza el TOC actual. */
typedef struct opd {
    u64 entry;
    u64 toc;
    u64 env;
} opd_t;

static opd_t jit_opd;

static int call_code(const volatile u32 *code, int arg)
{
    u64 toc;
    __asm__ volatile("mr %0,2" : "=r"(toc));
    jit_opd.entry = (u64)(uintptr_t)code;
    jit_opd.toc = toc;
    jit_opd.env = 0;
    __asm__ volatile("" : : : "memory");
    int (*fn)(int) = (int (*)(int))(void *)&jit_opd;
    return fn(arg);
}

/* Escribe, ejecuta, modifica y vuelve a ejecutar en `code`. */
static int write_run_modify(volatile u32 *code, const char *where)
{
    emit(code, 7);
    flush_code((const void *)code, 12);
    int r = call_code(code, 5);
    if (r != 22) {
        LOGE("%s: f(5) = %d, esperado 22", where, r);
        return -1;
    }
    emit(code, 9);
    flush_code((const void *)code, 12);
    r = call_code(code, 5);
    if (r != 24) {
        LOGE("%s: tras modificar, f(5) = %d, esperado 24 (instrucciones viejas en la cache)", where, r);
        return -1;
    }
    LOGI("%s: f(5) = 22 y, modificada, 24", where);
    return 0;
}

static int test_jit(const ps3_env_t *env)
{
    volatile u32 *nb = gcps3_jit_nobits;

    LOGI("cola NOBITS: %u MB en %p (dynarec segun PS3MAPI: %s)", NOBITS_MB, (void *)gcps3_jit_nobits,
         ps3_env_supports_dynarec(env) ? "si" : "no");
    sysLv2FsMkdir(DATA_DIR, 0777);
    if (previous_crash())
        return -1;

    step_begin("leer");
    uint32_t nonzero = 0;
    const uint64_t t0 = udplog_now_us();
    for (uint32_t k = 0; k < NOBITS_BYTES / 4; k++)
        if (nb[k] != 0)
            nonzero++;
    LOGI("leer: %u MB en %llu us, %u palabras distintas de cero", NOBITS_MB,
         (unsigned long long)(udplog_now_us() - t0), nonzero);
    step_ok("leer");
    if (nonzero) {
        LOGE("la cola no vale cero: el cargador no la relleno");
        return -1;
    }

    step_begin("ejecutar y modificar");
    if (write_run_modify(nb, "inicio") != 0)
        return -1;
    step_ok("ejecutar y modificar");

    step_begin("final");
    if (write_run_modify(nb + (NOBITS_BYTES - TAIL_BYTES) / 4, "final") != 0)
        return -1;
    step_ok("final");

    step_begin("limpieza");
    const uint64_t t1 = udplog_now_us();
    for (int i = 0; i < FLUSH_REPS; i++)
        flush_code((const void *)nb, FLUSH_BYTES);
    LOGI("limpiar 64 KB de cache (dcbst/sync/icbi/isync): %.2f us", (udplog_now_us() - t1) / (double)FLUSH_REPS);
    step_ok("limpieza");

    sysLv2FsUnlink(JIT_STATE);
    return 0;
}

int main(void)
{
    return phase0_main("jit", test_jit);
}
