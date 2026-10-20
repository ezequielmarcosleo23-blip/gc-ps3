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
 * DOL de prueba "contador": un contador que mueve la interrupción de video.
 * El manejador de la interrupción externa (copiado a 0x500) borra DI0 y DI1,
 * cuenta y anota el timebase. main dibuja la cuenta en pantalla cada vez que
 * cambia y, a las 9 interrupciones, mide el período entre campos con el
 * timebase: en 480i NTSC son 525 medias líneas de 15 444 ciclos, 675 675
 * ticks (59,94 campos por segundo). Después sigue contando en pantalla.
 *
 * Solo para el DOL: tests/dol/esperado/contador.txt está escrito a mano.
 */
#include "rt.h"
#include "video.h"

#define VECTOR   0x80000500u
#define MEASURED 9u                              /* interrupciones: 8 períodos */
#define PERIOD   675675u                         /* ticks de timebase por campo */
#define TB_HZ    40500000u

#define PI_INTMR (*(volatile uint32_t *)0xCC003004u)
#define PI_INT_VI 0x100u

volatile uint32_t vi_count;
volatile uint32_t vi_tb;                         /* TBL de la última interrupción */

/* Corre con la traducción apagada (MSR[DR] = 0): la VI en su dirección física
 * (0x0C002000) y las variables restando 0x80000000. Usa r3 y r4 (guardados en
 * SPRG0 y SPRG1) y no toca CR. Se copia a 0x500 tal cual. */
extern const uint32_t ext_handler[], ext_handler_end[];
__asm__("    .section .text\n"
        "    .globl ext_handler, ext_handler_end\n"
        "ext_handler:\n"
        "    mtsprg  0, 3\n"
        "    mtsprg  1, 4\n"
        "    lis     3, 0x0C00\n"
        "    lhz     4, 0x2030(3)\n"             /* DI0: INT es el bit de arriba */
        "    clrlwi  4, 4, 17\n"
        "    sth     4, 0x2030(3)\n"
        "    lhz     4, 0x2034(3)\n"             /* DI1 */
        "    clrlwi  4, 4, 17\n"
        "    sth     4, 0x2034(3)\n"
        "    lis     3, (vi_count - 0x80000000)@ha\n"
        "    lwz     4, (vi_count - 0x80000000)@l(3)\n"
        "    addi    4, 4, 1\n"
        "    stw     4, (vi_count - 0x80000000)@l(3)\n"
        "    mftb    4\n"
        "    lis     3, (vi_tb - 0x80000000)@ha\n"
        "    stw     4, (vi_tb - 0x80000000)@l(3)\n"
        "    mfsprg  4, 1\n"
        "    mfsprg  3, 0\n"
        "    rfi\n"
        "ext_handler_end:\n");

static void install(void)
{
    volatile uint32_t *dst = (volatile uint32_t *)VECTOR;
    const uint32_t n = (uint32_t)(ext_handler_end - ext_handler);
    for (uint32_t i = 0; i < n; i++)
        dst[i] = ext_handler[i];
    for (uint32_t a = VECTOR; a < VECTOR + 4 * n; a += 32)
        __asm__ volatile("dcbst 0, %0\n\tsync\n\ticbi 0, %0" : : "r"(a) : "memory");
    __asm__ volatile("sync\n\tisync" : : : "memory");
}

/* "campo N" centrado, borrando lo anterior */
static void show(uint32_t n)
{
    char text[20] = "campo ", digits[11];
    uint32_t len = 0, at = 6;
    do {
        digits[len++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (len)
        text[at++] = digits[--len];
    text[at] = '\0';
    video_rect(0, 200, XFB_PAIRS, 64, COLOR_FONDO);
    video_text((XFB_PAIRS - video_text_pairs(text, 4)) / 2, 200, 4, COLOR_BLANCO, text);
    video_flush();
}

int main(void)
{
    static const char title[] = "gc-ps3 fase 1: contador";
    video_fill(COLOR_FONDO);
    video_text((XFB_PAIRS - video_text_pairs(title, 1)) / 2, 80, 1, COLOR_AMARILLO, title);
    show(0);
    install();
    video_init_480i();                           /* DI0 y DI1 sin INT, habilitadas */
    PI_INTMR = PI_INT_VI;
    OSReport("contador: VI en 480i NTSC, DI0 y DI1 una vez por campo\n");

    uint32_t msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr | 0x8000u));     /* EE */

    uint32_t shown = 0, tb_first = 0;
    for (;;) {
        const uint32_t n = vi_count;
        if (n == shown)
            continue;
        if (n == 1)
            tb_first = vi_tb;
        if (n == MEASURED) {
            const uint32_t elapsed = vi_tb - tb_first;
            const uint32_t avg = (elapsed + (MEASURED - 1) / 2) / (MEASURED - 1);
            const uint32_t hz100 = TB_HZ / avg * 100 + TB_HZ % avg * 100 / avg;
            const int ok = avg + PERIOD / 1000 >= PERIOD && avg <= PERIOD + PERIOD / 1000;
            OSReport("contador: %u interrupciones de video\n", (unsigned)n);
            OSReport("contador: periodo %u ticks, %u.%02u campos por segundo: %s\n", (unsigned)avg,
                     (unsigned)(hz100 / 100), (unsigned)(hz100 % 100), ok ? "OK" : "FALLO");
        }
        show(n);
        shown = n;
    }
}
