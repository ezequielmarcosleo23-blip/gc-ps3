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
 * DOL de prueba "timer": instala un manejador del decrementador en 0x900,
 * prende MSR[EE] y cuenta 10 interrupciones. Comprueba que el tiempo medido
 * con el timebase sea el esperado (10 x (PERIODO + 1) ticks, con margen).
 *
 * Solo para el DOL: no tiene versión para la PC (tests/dol/esperado/timer.txt
 * está escrito a mano).
 */
#include "rt.h"

#define PERIOD   2000u                  /* ticks de timebase entre interrupciones */
#define WANTED   10u
#define VECTOR   0x80000900u

volatile uint32_t ticks;
volatile uint32_t period = PERIOD;

/* El manejador corre con la traducción apagada (MSR[DR] = 0): usa las
 * direcciones físicas de ticks y period. Se copia a 0x900 tal cual. */
extern const uint32_t dec_handler[], dec_handler_end[];
__asm__("    .section .text\n"
        "    .globl dec_handler, dec_handler_end\n"
        "dec_handler:\n"
        "    mtsprg  0, 3\n"
        "    mtsprg  1, 4\n"
        "    lis     3, (ticks - 0x80000000)@ha\n"
        "    lwz     4, (ticks - 0x80000000)@l(3)\n"
        "    addi    4, 4, 1\n"
        "    stw     4, (ticks - 0x80000000)@l(3)\n"
        "    lis     3, (period - 0x80000000)@ha\n"
        "    lwz     4, (period - 0x80000000)@l(3)\n"
        "    mtdec   4\n"
        "    mfsprg  4, 1\n"
        "    mfsprg  3, 0\n"
        "    rfi\n"
        "dec_handler_end:\n");

static void install(void)
{
    volatile uint32_t *dst = (volatile uint32_t *)VECTOR;
    const uint32_t n = (uint32_t)(dec_handler_end - dec_handler);
    for (uint32_t i = 0; i < n; i++)
        dst[i] = dec_handler[i];
    for (uint32_t a = VECTOR; a < VECTOR + 4 * n; a += 32)
        __asm__ volatile("dcbst 0, %0\n\tsync\n\ticbi 0, %0" : : "r"(a) : "memory");
    __asm__ volatile("sync\n\tisync" : : : "memory");
}

static uint64_t timebase(void)
{
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0\n\tmftb %1\n\tmftbu %2" : "=r"(hi), "=r"(lo), "=r"(hi2));
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}

int main(void)
{
    uint32_t msr;
    OSReport("timer: periodo %u ticks, espero %u interrupciones\n", PERIOD, WANTED);
    install();

    ticks = 0;
    const uint64_t t0 = timebase();
    __asm__ volatile("mtdec %0" : : "r"(PERIOD));
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr | 0x8000u));     /* EE */

    uint32_t spins = 0;
    while (ticks < WANTED && spins < 50000000u)
        spins++;
    const uint64_t t1 = timebase();
    __asm__ volatile("mtmsr %0" : : "r"(msr));

    const uint64_t elapsed = t1 - t0, ideal = (uint64_t)WANTED * (PERIOD + 1);
    const int in_range = elapsed + ideal / 20 >= ideal && elapsed <= ideal + ideal / 20;
    OSReport("timer: %u interrupciones\n", (unsigned)ticks);
    OSReport("timer: tiempo %s (%s del 5%% de %u ticks)\n", in_range ? "OK" : "FALLO",
             in_range ? "dentro" : "fuera", (unsigned)ideal);
    return ticks == WANTED && in_range ? 0 : 1;
}
