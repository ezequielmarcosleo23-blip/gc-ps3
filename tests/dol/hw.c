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
 * DOL de prueba "hw": el hardware alrededor de la CPU, tocando los registros
 * a mano. Revisión del PI, mandos por el SI (comando de estado y sondeo de la
 * VI), reloj y SRAM del IPL por EXI, la UART del IPL, ida y vuelta a la ARAM
 * por DMA, el correo del DSP al arrancar, el DVD sin disco y el contador de
 * muestras del AI. Cada línea dice OK o FALLO; solo imprime valores que dan
 * igual en cualquier emulador fiel (nada de la hora ni de cuántas vueltas
 * dio un bucle).
 *
 * Solo para el DOL: tests/dol/esperado/hw.txt está escrito a mano.
 */
#include "rt.h"

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))

#define PI_FLIPPER_REV 0xCC00302Cu
#define SI_COMCSR      0xCC006434u
#define SI_SR          0xCC006438u
#define SI_IN0_HI      0xCC006404u
#define SI_IN0_LO      0xCC006408u
#define SI_BUF         0xCC006480u
#define EXI0_CSR       0xCC006800u
#define EXI0_MAR       0xCC006804u
#define EXI0_LENGTH    0xCC006808u
#define EXI0_CR        0xCC00680Cu
#define EXI0_DATA      0xCC006810u
#define DSP_MAIL_HI    0xCC005004u
#define DSP_MAIL_LO    0xCC005006u
#define DSP_CSR        0xCC00500Au
#define AR_MM_HI       0xCC005020u
#define AR_MM_LO       0xCC005022u
#define AR_AR_HI       0xCC005024u
#define AR_AR_LO       0xCC005026u
#define AR_CNT_HI      0xCC005028u
#define AR_CNT_LO      0xCC00502Au
#define DI_CVR         0xCC006004u
#define AI_CR          0xCC006C00u
#define AI_SCNT        0xCC006C08u
#define OS_SYSTEM_TIME 0x800030D8u        /* ticks de timebase desde 2000 (memoria baja) */

#define TB_HZ          40500000u
#define SPIN_MAX       2000000u

static unsigned failures;

static void check(const char *line, int ok)
{
    OSReport("hw: %s: %s\n", line, ok ? "OK" : "FALLO");
    if (!ok)
        failures++;
}

static uint64_t timebase(void)
{
    uint32_t hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0\n\tmftb %1\n\tmftbu %2" : "=r"(hi), "=r"(lo), "=r"(hi2));
    } while (hi != hi2);
    return (uint64_t)hi << 32 | lo;
}

/* Caché de datos: escribir antes de que lea un DMA, descartar antes de leer lo que escribió */
static void cache_flush(const void *p, uint32_t len)
{
    for (uint32_t a = (uint32_t)p & ~31u; a < (uint32_t)p + len; a += 32)
        __asm__ volatile("dcbf 0, %0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

static void cache_invalidate(const void *p, uint32_t len)
{
    for (uint32_t a = (uint32_t)p & ~31u; a < (uint32_t)p + len; a += 32)
        __asm__ volatile("dcbi 0, %0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

static uint32_t phys(const void *p) { return (uint32_t)p & 0x3FFFFFFFu; }

/* ---- SI ---------------------------------------------------------------------------- */

/* Comando 0x00 (estado) por el búfer: los 3 bytes de tipo, o ~0 si no hay nada */
static uint32_t si_status(uint32_t port)
{
    REG32(SI_BUF) = 0x00000000u;
    REG32(SI_COMCSR) = 1u << 16 | 3u << 8 | port << 1 | 1u;    /* sale 1 byte, entran 3 */
    for (uint32_t n = 0; (REG32(SI_COMCSR) & 1u) && n < SPIN_MAX; n++)
        ;
    const uint32_t csr = REG32(SI_COMCSR);
    REG32(SI_COMCSR) = 0x80000000u;                            /* borra TCINT */
    REG32(SI_SR) = 0x0F0F0F0Fu;                                /* y los errores */
    if (csr & 0x20000000u)                                     /* COMERR: sin respuesta */
        return 0xFFFFFFFFu;
    return REG32(SI_BUF) >> 8;
}

static void test_si(void)
{
    const uint32_t t1 = si_status(0);
    int empty = 1;
    for (uint32_t p = 1; p < 4; p++)
        if (si_status(p) != 0xFFFFFFFFu)
            empty = 0;
    OSReport("hw: si: puerto 1 tipo 0x%06x\n", (unsigned)t1);
    check("si: mando estandar en el puerto 1, puertos 2 a 4 vacios", t1 == 0x090000u && empty);

    /* La VI sondea los puertos una vez por campo: esperar RDST del puerto 1 */
    (void)REG32(SI_IN0_HI);                                    /* leer borra RDST */
    const uint64_t t0 = timebase();
    while (!(REG32(SI_SR) & 0x20000000u) && timebase() - t0 < TB_HZ / 10)
        ;
    const uint32_t hi = REG32(SI_IN0_HI), lo = REG32(SI_IN0_LO);
    OSReport("hw: si: sondeo del puerto 1 0x%08x 0x%08x\n", (unsigned)hi, (unsigned)lo);
    check("si: sin botones y con las palancas al centro", hi == 0x00808080u && lo == 0x80800000u);
}

/* ---- EXI: el chip del IPL (canal 0, CS 2) --------------------------------------- */

static void exi_select(uint32_t cs) { REG32(EXI0_CSR) = cs << 7 | 3u << 4; }  /* 8 MHz */

static void exi_wait(void)
{
    for (uint32_t n = 0; (REG32(EXI0_CR) & 1u) && n < SPIN_MAX; n++)
        ;
}

static void exi_write(uint32_t data, uint32_t len)
{
    REG32(EXI0_DATA) = data;
    REG32(EXI0_CR) = (len - 1) << 4 | 1u << 2 | 1u;
    exi_wait();
}

static uint32_t exi_read(uint32_t len)
{
    REG32(EXI0_CR) = (len - 1) << 4 | 1u;
    exi_wait();
    return REG32(EXI0_DATA);
}

static uint8_t sram[64] __attribute__((aligned(32)));

static void test_exi(void)
{
    exi_select(2);
    exi_write(0x20000000u, 4);                                 /* reloj */
    const uint32_t rtc = exi_read(4);
    exi_select(0);
    const uint32_t os = (uint32_t)(*(volatile uint64_t *)OS_SYSTEM_TIME / TB_HZ);
    check("exi: el reloj del IPL coincide con la hora del sistema",
          rtc + 2 >= os && rtc <= os + 2);

    cache_invalidate(sram, sizeof sram);
    exi_select(2);
    exi_write(0x20000100u, 4);                                 /* SRAM desde el byte 4 */
    REG32(EXI0_MAR) = phys(sram);
    REG32(EXI0_LENGTH) = sizeof sram;
    REG32(EXI0_CR) = 0x3u;                                     /* DMA de lectura */
    exi_wait();
    exi_select(0);
    cache_invalidate(sram, sizeof sram);
    uint16_t sum = 0, inv = 0;
    for (uint32_t k = 0x0C; k < 0x14; k += 2) {
        const uint16_t v = (uint16_t)(sram[k] << 8 | sram[k + 1]);
        sum = (uint16_t)(sum + v);
        inv = (uint16_t)(inv + (uint16_t)~v);
    }
    const uint16_t want = (uint16_t)(sram[0] << 8 | sram[1]);
    const uint16_t want_inv = (uint16_t)(sram[2] << 8 | sram[3]);
    OSReport("hw: exi: SRAM suma 0x%04x/0x%04x, idioma %u, banderas 0x%02x\n", (unsigned)want,
             (unsigned)want_inv, (unsigned)sram[0x12], (unsigned)sram[0x13]);
    check("exi: suma de control de la SRAM", sum == want && inv == want_inv);

    /* La UART de depuración del IPL: el texto sale por el log como OSReport */
    static const char msg[] = "hw: hola por la UART del IPL\r";
    exi_select(2);
    exi_write(0xA0010000u, 4);
    for (uint32_t k = 0; k < sizeof msg - 1; k += 4) {
        uint32_t w = 0, n = 0;
        for (; n < 4 && k + n < sizeof msg - 1; n++)
            w |= (uint32_t)(uint8_t)msg[k + n] << (24 - 8 * n);
        exi_write(w, n);
    }
    exi_select(0);
}

/* ---- ARAM y DSP ---------------------------------------------------------------------- */

static uint8_t buf[1024] __attribute__((aligned(32)));

static void aram_dma(uint32_t to_ram, uint32_t aram)
{
    REG16(AR_MM_HI) = (uint16_t)(phys(buf) >> 16);
    REG16(AR_MM_LO) = (uint16_t)phys(buf);
    REG16(AR_AR_HI) = (uint16_t)(aram >> 16);
    REG16(AR_AR_LO) = (uint16_t)aram;
    REG16(AR_CNT_HI) = (uint16_t)(to_ram ? 0x8000u : 0);
    REG16(AR_CNT_LO) = sizeof buf;                             /* arranca */
    for (uint32_t n = 0; (REG16(DSP_CSR) & 0x0200u) && n < SPIN_MAX; n++)
        ;
}

static void test_aram_dsp(void)
{
    for (uint32_t k = 0; k < sizeof buf; k++)
        buf[k] = (uint8_t)(k * 13 + 7);
    cache_flush(buf, sizeof buf);
    aram_dma(0, 0x4000);
    const int ar_int = (REG16(DSP_CSR) & 0x0020u) != 0;
    REG16(DSP_CSR) = REG16(DSP_CSR);                           /* borra ARINT */
    for (uint32_t k = 0; k < sizeof buf; k++)
        buf[k] = 0;
    cache_flush(buf, sizeof buf);
    aram_dma(1, 0x4000);
    cache_invalidate(buf, sizeof buf);
    int same = 1;
    for (uint32_t k = 0; k < sizeof buf; k++)
        if (buf[k] != (uint8_t)(k * 13 + 7))
            same = 0;
    check("aram: 1024 bytes de ida y vuelta por DMA, con ARINT", same && ar_int);
    REG16(DSP_CSR) = REG16(DSP_CSR);

    const uint16_t csr = REG16(DSP_CSR);
    REG16(DSP_CSR) = 0x0800u;                                  /* sale de la pausa (DSPInit sigue) */
    const uint32_t mail = (uint32_t)REG16(DSP_MAIL_HI) << 16 | REG16(DSP_MAIL_LO);
    OSReport("hw: dsp: DSPCSR 0x%04x, primer correo 0x%08x\n", (unsigned)csr, (unsigned)mail);
    check("dsp: el ROM del DSP saluda al arrancar", csr == 0x0804u && mail == 0x8071FEEDu);
}

/* ---- DVD y audio --------------------------------------------------------------------- */

static void test_di_ai(void)
{
    check("di: sin disco, la tapa abierta", (REG32(DI_CVR) & 1u) != 0);

    REG32(AI_CR) = 0x43u;                                      /* PSTAT, AIS 48 kHz, AID 32 kHz */
    const uint64_t t0 = timebase();
    while (timebase() - t0 < TB_HZ / 100)                      /* 10 ms */
        ;
    const uint32_t samples = REG32(AI_SCNT);
    REG32(AI_CR) = 0x42u;
    check("ai: el contador de muestras avanza a 48 kHz", samples >= 470 && samples <= 490);
}

int main(void)
{
    const uint32_t rev = REG32(PI_FLIPPER_REV);
    OSReport("hw: pi: revision de Flipper 0x%08x\n", (unsigned)rev);
    check("pi: Flipper de las consolas de venta", rev == 0x246500B1u);
    test_si();
    test_exi();
    test_aram_dsp();
    test_di_ai();
    if (failures)
        OSReport("hw: %u FALLOS\n", failures);
    else
        OSReport("hw: todo OK\n");
    return failures ? 1 : 0;
}
