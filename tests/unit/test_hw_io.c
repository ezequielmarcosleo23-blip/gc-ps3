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

/* SI, EXI (IPL), DSP y ARAM, AI, DI, MI, CP/PE y el FIFO de GX. */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "dol_build.h"
#include "test.h"

#define TEXT0 0x80003100u
#define IO    0xCC000000u

static gc_system_t sys;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

/* li r3,0; bucle: addi r3,r3,1; b bucle */
static const uint32_t spin[] = { 0x38600000u, 0x38630001u, 0x4BFFFFFCu };

static int boot(void)
{
    static dolb_t b;
    const gc_boot_config_t cfg = { 0, 100 };          /* reloj: 100 s después de 2000 */
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    dolb_init(&b);
    dolb_code(&b, 0, TEXT0, spin, 3);
    dolb_entry(&b, TEXT0);
    return gc_system_boot_dol(&sys, b.buf, b.len, &cfg) == GC_DOL_OK ? 0 : -1;
}

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

static uint32_t r32(uint32_t a) { return gc_read32(&sys.mem, IO + a); }
static uint32_t r16(uint32_t a) { return gc_read16(&sys.mem, IO + a); }
static void w32(uint32_t a, uint32_t v) { gc_write32(&sys.mem, IO + a, v); }
static void w16(uint32_t a, uint32_t v) { gc_write16(&sys.mem, IO + a, (uint16_t)v); }
static int pi_cause(uint32_t bit) { return (r32(0x3000) & bit) != 0; }

static void run_to(uint64_t t)
{
    if (t > sys.cpu.cycles)
        gc_system_run(&sys, t - sys.cpu.cycles);
}

/* ---- SI --------------------------------------------------------------------------- */

#define SI 0x6400u

/* Un comando por el búfer: largo de salida y de entrada, canal */
static void si_command(int c, uint32_t first_word, uint32_t out_len, uint32_t in_len)
{
    w32(SI + SI_BUF, first_word);
    w32(SI + SI_COMCSR, SI_COMCSR_TCINTMSK | out_len << 16 | in_len << 8 | (uint32_t)c << 1 |
                            SI_COMCSR_TSTART);
}

static void test_si_commands(void)
{
    CHECK(boot() == 0);
    CHECK_EQ_U64(r32(SI + SI_POLL), 0x01EC0000u);
    CHECK_EQ_U64(r32(SI + SI_COMCSR), 0);

    si_command(0, 0x00000000u, 1, 3);                  /* estado: mando estándar */
    CHECK_EQ_U64(r32(SI + SI_COMCSR) & (SI_COMCSR_TSTART | SI_COMCSR_COMERR | SI_COMCSR_TCINT),
                 SI_COMCSR_TCINT);
    CHECK_EQ_U64(r32(SI + SI_BUF), 0x09000000u);
    CHECK_EQ_U64(r16(SI + SI_BUF), 0x0900u);
    CHECK(pi_cause(PI_INT_SI));
    w32(SI + SI_COMCSR, SI_COMCSR_TCINT | SI_COMCSR_TCINTMSK);   /* 1 borra */
    CHECK(!pi_cause(PI_INT_SI));

    si_command(1, 0x00000000u, 1, 3);                  /* puerto 2: vacío */
    CHECK(r32(SI + SI_COMCSR) & SI_COMCSR_COMERR);
    CHECK_EQ_U64(r32(SI + SI_SR), SI_SR_NOREP(1));
    w32(SI + SI_SR, SI_SR_NOREP(1));
    CHECK_EQ_U64(r32(SI + SI_SR), 0);

    si_command(0, 0x41000000u, 1, 10);                 /* origen: palancas al centro */
    CHECK_EQ_U64(r32(SI + SI_BUF), 0x00008080u);
    CHECK_EQ_U64(r32(SI + SI_BUF + 4), 0x80800000u);
    CHECK_EQ_U64(r16(SI + SI_BUF + 8), 0x0000u);

    sys.si.pad[0].buttons = 0x0100;                    /* A */
    sys.si.pad[0].trig_l = 0xA0;
    sys.si.pad[0].trig_r = 0x50;
    sys.si.pad[0].analog_a = 0x30;
    sys.si.pad[0].analog_b = 0x20;
    si_command(0, 0x40000300u, 3, 8);                  /* directo, modo 3 */
    CHECK_EQ_U64(r32(SI + SI_BUF), 0x01808080u);
    CHECK_EQ_U64(r32(SI + SI_BUF + 4), 0x8080A050u);

    w32(SI + SI_OUT0, 0x00400000u);                    /* modo 0 por SIOUT y WR */
    w32(SI + SI_SR, SI_SR_WR);
    CHECK_EQ_U64(sys.si.mode[0], 0);
    CHECK(!(r32(SI + SI_SR) & SI_SR_WR));
    si_command(0, 0x40000000u, 3, 8);
    CHECK_EQ_U64(r32(SI + SI_BUF + 4), 0x8080A532u);
    teardown();
}

/* La VI sondea los mandos 15 medias líneas después del comienzo de cada campo. */
static void test_si_poll(void)
{
    CHECK(boot() == 0);
    w32(SI + SI_COMCSR, SI_COMCSR_RDSTINTMSK);
    run_to(7722u * 16);                                /* justo antes de la media línea 15 */
    CHECK_EQ_U64(r32(SI + SI_SR), 0);
    gc_system_run(&sys, 1);
    CHECK_EQ_U64(r32(SI + SI_SR), SI_SR_RDST(0) | SI_SR_NOREP(1) | SI_SR_NOREP(2) | SI_SR_NOREP(3));
    CHECK(r32(SI + SI_COMCSR) & SI_COMCSR_RDSTINT);
    CHECK(pi_cause(PI_INT_SI));
    CHECK_EQ_U64(r32(SI + SI_IN0_HI + 0x0C), 0xC0000000u);     /* puerto 2: error */
    CHECK_EQ_U64(r32(SI + SI_IN0_LO), 0x80800000u);            /* leer borra RDST */
    CHECK(!(r32(SI + SI_SR) & SI_SR_RDST(0)));
    CHECK(!pi_cause(PI_INT_SI));
    CHECK_EQ_U64(r32(SI + SI_IN0_HI), 0x00808080u);
    run_to(7722u * 541 + 1);                           /* el del campo par */
    CHECK(r32(SI + SI_SR) & SI_SR_RDST(0));
    teardown();
}

/* ---- EXI -------------------------------------------------------------------------- */

#define EXI 0x6800u

static void exi_select(int c, uint32_t cs) { w32(EXI + 0x14 * (uint32_t)c, cs << EXI_CSR_CS_SHIFT); }

static void exi_imm_write(int c, uint32_t data, uint32_t len)
{
    w32(EXI + 0x14 * (uint32_t)c + EXI_DATA, data);
    w32(EXI + 0x14 * (uint32_t)c + EXI_CR, (len - 1) << 4 | 1u << 2 | EXI_CR_TSTART);
}

static uint32_t exi_imm_read(int c, uint32_t len)
{
    w32(EXI + 0x14 * (uint32_t)c + EXI_CR, (len - 1) << 4 | EXI_CR_TSTART);
    return r32(EXI + 0x14 * (uint32_t)c + EXI_DATA);
}

static void test_exi_rtc_sram(void)
{
    CHECK(boot() == 0);
    CHECK_EQ_U64(r32(EXI + EXI_CSR), EXI_CSR_EXTINT);
    CHECK_EQ_U64(r32(EXI + 0x14 + EXI_CSR), EXI_CSR_EXTINT | 1u << EXI_CSR_CS_SHIFT);
    CHECK_EQ_U64(r32(EXI + 0x28 + EXI_CSR), 0);

    exi_select(0, 2);                                  /* chip del IPL */
    exi_imm_write(0, EXI_IPL_RTC, 4);
    CHECK(r32(EXI + EXI_CSR) & EXI_CSR_TCINT);
    CHECK_EQ_U64(exi_imm_read(0, 4), 100);
    exi_select(0, 0);

    sys.cpu.cycles += 3ull * GC_CPU_CLOCK;             /* 3 s emulados después (sin correr) */
    exi_select(0, 2);
    exi_imm_write(0, EXI_IPL_RTC, 4);
    CHECK_EQ_U64(exi_imm_read(0, 4), 103);
    exi_select(0, 0);

    /* SRAM por DMA: 64 bytes desde el byte 4 */
    exi_select(0, 2);
    exi_imm_write(0, EXI_IPL_SRAM, 4);
    w32(EXI + EXI_MAR, 0x00001000u);
    w32(EXI + EXI_LENGTH, 64);
    w32(EXI + EXI_CR, EXI_CR_DMA | EXI_CR_TSTART);
    exi_select(0, 0);
    gc_mem_t *m = &sys.mem;
    CHECK_EQ_U64(gc_read32(m, 0x80001000u), 0x002CFFD0u);  /* suma y su complemento */
    CHECK_EQ_U64(gc_read32(m, 0x80001010u), 0x0000002Cu);  /* idioma 0, banderas 0x2C */
    CHECK(memcmp(gc_ram_ptr(m, 0x80001014u, 12), "DOLPHINSLOTA", 12) == 0);
    uint16_t sum = 0, inv = 0;
    for (uint32_t k = 0x0C; k < 0x14; k += 2) {
        sum = (uint16_t)(sum + gc_read16(m, 0x80001000u + k));
        inv = (uint16_t)(inv + (uint16_t)~gc_read16(m, 0x80001000u + k));
    }
    CHECK_EQ_U64(sum, 0x002C);
    CHECK_EQ_U64(inv, 0xFFD0);

    /* Escritura y lectura de vuelta (el idioma, byte 0x16) */
    exi_select(0, 2);
    exi_imm_write(0, 0x80000000u | (0x800016u << 6), 4);
    exi_imm_write(0, 0x05000000u, 1);
    exi_select(0, 0);
    CHECK_EQ_U64(sys.exi.sram[0x16], 5);
    exi_select(0, 2);
    exi_imm_write(0, 0x800016u << 6, 4);
    CHECK_EQ_U64(exi_imm_read(0, 2), 0x052C0000u);
    exi_select(0, 0);
    teardown();
}

static char uart_line[64];

static void uart_report(void *ctx, const char *line)
{
    (void)ctx;
    strncpy(uart_line, line, sizeof uart_line - 1);
}

static void test_exi_uart_irq_slots(void)
{
    CHECK(boot() == 0);
    sys.hle.report = uart_report;
    uart_line[0] = '\0';
    exi_select(0, 2);
    exi_imm_write(0, 0x80000000u | EXI_IPL_UART, 4);
    exi_imm_write(0, 0x686F6C61u, 4);                  /* "hola" */
    exi_imm_write(0, 0x0D000000u, 1);                  /* \r */
    exi_select(0, 0);
    CHECK(strcmp(uart_line, "hola") == 0);

    /* Interrupción de fin de transferencia por el PI */
    w32(EXI + EXI_CSR, EXI_CSR_TCINT | EXI_CSR_EXTINT);           /* borra lo pendiente */
    CHECK(!pi_cause(PI_INT_EXI));
    w32(EXI + EXI_CSR, EXI_CSR_TCINTMASK | 2u << EXI_CSR_CS_SHIFT);
    exi_imm_write(0, EXI_IPL_RTC, 4);
    CHECK(pi_cause(PI_INT_EXI));
    w32(EXI + EXI_CSR, EXI_CSR_TCINTMASK | EXI_CSR_TCINT);
    CHECK(!pi_cause(PI_INT_EXI));

    /* Ranuras de tarjeta vacías: nada contesta, EXT en 0 */
    exi_select(1, 1);
    CHECK_EQ_U64(exi_imm_read(1, 4), 0);
    CHECK(!(r32(EXI + 0x14) & EXI_CSR_EXT));
    exi_select(1, 0);
    w32(EXI + 0x14 + EXI_CSR, EXI_CSR_EXTINTMASK);     /* EXTINT del encendido */
    CHECK(pi_cause(PI_INT_EXI));
    teardown();
}

/* ---- DSP y ARAM -------------------------------------------------------------------- */

#define DSP 0x5000u

static void test_dsp_mail(void)
{
    CHECK(boot() == 0);
    CHECK_EQ_U64(r16(DSP + DSP_CSR), DSP_CSR_HALT | DSP_CSR_INIT);
    CHECK_EQ_U64(r16(DSP + DSP_MAIL_FROM_HI), 0);      /* detenido: no hay correo */
    CHECK_EQ_U64(r16(DSP + DSP_MAIL_FROM_LO), 0);

    w16(DSP + DSP_CSR, DSP_CSR_INIT);                  /* arranca: el correo del ROM */
    CHECK_EQ_U64(r32(DSP + DSP_MAIL_FROM_HI), DSP_MAIL_ROM);
    CHECK_EQ_U64(r16(DSP + DSP_MAIL_FROM_HI), 0x0071u);  /* leído: sin el bit de arriba */

    w16(DSP + DSP_MAIL_TO_HI, 0x8000);                 /* el ROM contesta 0xFEEExxxx */
    w16(DSP + DSP_MAIL_TO_LO, 0x1234);
    CHECK_EQ_U64(r32(DSP + DSP_MAIL_TO_HI), 0x00001234u);
    CHECK_EQ_U64(r32(DSP + DSP_MAIL_FROM_HI), 0xFEEE1234u);

    w16(DSP + DSP_CSR, DSP_CSR_INIT | DSP_CSR_RESET);  /* reset: otra vez el ROM */
    CHECK_EQ_U64(r16(DSP + DSP_CSR), DSP_CSR_INIT);
    CHECK_EQ_U64(r32(DSP + DSP_MAIL_FROM_HI), DSP_MAIL_ROM);

    w16(DSP + DSP_CSR, 0);                             /* baja DSPInit: microcódigo INIT */
    CHECK_EQ_U64(r16(DSP + DSP_CSR), DSP_CSR_INITCODE);
    CHECK_EQ_U64(r32(DSP + DSP_MAIL_FROM_HI), DSP_MAIL_INIT);
    run_to(sys.cpu.cycles + 130 * 12 + 12);
    CHECK_EQ_U64(r16(DSP + DSP_CSR), 0);
    teardown();
}

static void test_dsp_aram_dma(void)
{
    CHECK(boot() == 0);
    gc_mem_t *m = &sys.mem;
    for (uint32_t k = 0; k < 64; k++)
        gc_write8(m, 0x80002000u + k, (uint8_t)(k * 7 + 1));
    w16(DSP + DSP_CSR, DSP_CSR_HALT | DSP_CSR_INIT | DSP_CSR_ARMASK);
    w32(DSP + AR_MMADDR_HI, 0x00002000u);
    w32(DSP + AR_ARADDR_HI, 0x00000100u);
    w32(DSP + AR_CNT_HI, 64);                          /* RAM -> ARAM */
    CHECK(memcmp(m->aram + 0x100, gc_ram_ptr(m, 0x80002000u, 64), 64) == 0);
    CHECK(r16(DSP + DSP_CSR) & DSP_CSR_DMA);
    CHECK_EQ_U64(r32(DSP + AR_CNT_HI), 0);
    CHECK_EQ_U64(r32(DSP + AR_MMADDR_HI), 0x00002040u);
    const uint64_t t0 = sys.cpu.cycles;
    run_to(t0 + 2 * 246);                              /* 2 bloques de 32 */
    CHECK(r16(DSP + DSP_CSR) & DSP_CSR_DMA);
    gc_system_run(&sys, 1);
    CHECK(!(r16(DSP + DSP_CSR) & DSP_CSR_DMA));
    CHECK(r16(DSP + DSP_CSR) & DSP_CSR_ARINT);
    CHECK(pi_cause(PI_INT_DSP));
    w16(DSP + DSP_CSR, r16(DSP + DSP_CSR));            /* escribir lo leído borra ARINT */
    CHECK(!pi_cause(PI_INT_DSP));

    w32(DSP + AR_MMADDR_HI, 0x00003000u);              /* ARAM -> RAM */
    w32(DSP + AR_ARADDR_HI, 0x00000100u);
    w32(DSP + AR_CNT_HI, 0x80000000u | 64);
    CHECK(memcmp(gc_ram_ptr(m, 0x80003000u, 64), gc_ram_ptr(m, 0x80002000u, 64), 64) == 0);

    w16(DSP + AR_MODE, 0);                             /* solo lectura */
    CHECK_EQ_U64(r16(DSP + AR_MODE), 1);
    CHECK_EQ_U64(r16(DSP + AR_REFRESH), 156);
    teardown();
}

static void test_dsp_audio_dma(void)
{
    CHECK(boot() == 0);
    w16(DSP + DSP_CSR, DSP_CSR_HALT | DSP_CSR_INIT | DSP_CSR_AIDMASK);
    w32(DSP + AUDIO_DMA_START_HI, 0x00004000u);
    run_to(121392u + 10);                              /* el primer bloque, sin DMA prendido */
    CHECK_EQ_U64(sys.dsp.audio_dma_blocks, 0);
    const uint64_t t0 = sys.cpu.cycles;
    w16(DSP + AUDIO_DMA_CONTROL, 0x8002);              /* 2 bloques */
    CHECK_EQ_U64(r16(DSP + AUDIO_DMA_LEFT), 1);
    run_to(t0 + 201);                                  /* aviso del comienzo */
    CHECK(r16(DSP + DSP_CSR) & DSP_CSR_AIDINT);
    CHECK(pi_cause(PI_INT_DSP));
    w16(DSP + DSP_CSR, r16(DSP + DSP_CSR));
    run_to(2u * 121392u + 1);
    CHECK_EQ_U64(sys.dsp.audio_dma_blocks, 1);
    CHECK_EQ_U64(r16(DSP + AUDIO_DMA_LEFT), 0);
    CHECK(!(r16(DSP + DSP_CSR) & DSP_CSR_AIDINT));
    run_to(3u * 121392u + 1);                          /* fin de la lista: vuelve a empezar */
    CHECK(r16(DSP + DSP_CSR) & DSP_CSR_AIDINT);
    CHECK_EQ_U64(r16(DSP + AUDIO_DMA_LEFT), 1);
    teardown();
}

/* ---- AI ----------------------------------------------------------------------------- */

#define AI 0x6C00u

static void test_ai(void)
{
    CHECK(boot() == 0);
    CHECK_EQ_U64(r32(AI + AI_CR), AI_CR_AISFR | AI_CR_AIDFR);
    CHECK_EQ_U64(r32(AI + AI_SCNT), 0);
    run_to(100000);
    CHECK_EQ_U64(r32(AI + AI_SCNT), 0);                /* sin PSTAT no cuenta */

    w32(AI + AI_IT, 5);
    w32(AI + AI_CR, AI_CR_AISFR | AI_CR_AIDFR | AI_CR_AIINTMSK | AI_CR_AIINTVLD | AI_CR_PSTAT);
    const uint64_t t0 = sys.cpu.cycles;
    run_to(t0 + 3 * 10116 + 5);
    CHECK_EQ_U64(r32(AI + AI_SCNT), 3);
    CHECK(!pi_cause(PI_INT_AI));
    run_to(t0 + 5 * 10116 + 5);
    CHECK_EQ_U64(r32(AI + AI_SCNT), 5);
    CHECK(r32(AI + AI_CR) & AI_CR_AIINT);
    CHECK(pi_cause(PI_INT_AI));
    w32(AI + AI_CR, r32(AI + AI_CR));                  /* 1 en AIINT lo borra */
    CHECK(!pi_cause(PI_INT_AI));

    w32(AI + AI_CR, AI_CR_AISFR | AI_CR_AIDFR);        /* parar: el contador queda */
    const uint32_t stopped = r32(AI + AI_SCNT);
    run_to(sys.cpu.cycles + 50000);
    CHECK_EQ_U64(r32(AI + AI_SCNT), stopped);
    w32(AI + AI_CR, AI_CR_AISFR | AI_CR_AIDFR | AI_CR_SCRESET);
    CHECK_EQ_U64(r32(AI + AI_SCNT), 0);
    CHECK(!(r32(AI + AI_CR) & AI_CR_SCRESET));
    teardown();
}

/* ---- DI ----------------------------------------------------------------------------- */

#define DI 0x6000u

static void test_di(void)
{
    CHECK(boot() == 0);
    CHECK_EQ_U64(r32(DI + DI_CVR), DI_CVR_OPEN);
    CHECK_EQ_U64(r32(DI + DI_CFG), 1);
    w32(DI + DI_SR, DI_SR_TCINTMASK | DI_SR_DEINTMASK);

    w32(DI + DI_CMDBUF0, 0x12000000u);                 /* Inquiry */
    w32(DI + DI_MAR, 0x80005000u);
    w32(DI + DI_LENGTH, 0x20);
    w32(DI + DI_CR, 3);
    CHECK_EQ_U64(r32(DI + DI_MAR), 0x00005000u);
    CHECK_EQ_U64(gc_read32(&sys.mem, 0x80005004u), 0x20060526u);
    const uint64_t t0 = sys.cpu.cycles;
    run_to(t0 + 145800);
    CHECK(r32(DI + DI_CR) & DI_CR_TSTART);
    gc_system_run(&sys, 1);
    CHECK_EQ_U64(r32(DI + DI_CR), 2);
    CHECK(r32(DI + DI_SR) & DI_SR_TCINT);
    CHECK_EQ_U64(r32(DI + DI_MAR), 0x00005020u);
    CHECK_EQ_U64(r32(DI + DI_LENGTH), 0);
    CHECK(pi_cause(PI_INT_DI));
    w32(DI + DI_SR, DI_SR_TCINT | DI_SR_TCINTMASK | DI_SR_DEINTMASK);
    CHECK(!pi_cause(PI_INT_DI));

    w32(DI + DI_CMDBUF0, 0xA8000000u);                 /* leer: no hay disco */
    w32(DI + DI_CR, 3);
    run_to(sys.cpu.cycles + 145801);
    CHECK(r32(DI + DI_SR) & DI_SR_DEINT);
    CHECK(!(r32(DI + DI_SR) & DI_SR_TCINT));
    w32(DI + DI_SR, DI_SR_DEINT | DI_SR_TCINTMASK | DI_SR_DEINTMASK);

    w32(DI + DI_CMDBUF0, 0xE0000000u);                 /* RequestError: tapa abierta, sin medio */
    w32(DI + DI_CR, 1);
    CHECK_EQ_U64(r32(DI + DI_IMMBUF), 0x01023A00u);
    run_to(sys.cpu.cycles + 145801);
    w32(DI + DI_CR, 1);
    CHECK_EQ_U64(r32(DI + DI_IMMBUF), 0x01000000u);    /* el error se borró al leerlo */
    teardown();
}

/* ---- MI, CP, PE y el FIFO de GX ------------------------------------------------------- */

static void test_mi_gx(void)
{
    CHECK(boot() == 0);
    w32(0x4000, 0x12345678u);                          /* región 0 */
    CHECK_EQ_U64(r32(0x4000), 0x12345678u);
    w16(0x401C, 0x00FF);
    CHECK_EQ_U64(r16(0x401C), 0x00FF);
    w16(0x4012, 0x1111);                               /* no existe */
    CHECK_EQ_U64(r16(0x4012), 0);

    CHECK_EQ_U64(r16(0x0000), CP_SR_IDLE);
    w16(0x0020, 0xFFFF);                               /* base del FIFO: alineada a 32 */
    w16(0x0022, 0xFFFF);
    CHECK_EQ_U64(r32(0x0020), 0xFFE003FFu);
    w16(0x0002, 0x0011);
    CHECK_EQ_U64(r16(0x0002), 0x0011);

    w16(0x100A, 0x000F);                               /* PE: TOKEN y FINISH solo se escriben */
    CHECK_EQ_U64(r16(0x100A), 0x0003);
    CHECK(!pi_cause(PI_INT_PE_TOKEN | PI_INT_PE_FINISH | PI_INT_CP));

    for (int k = 0; k < 8; k++)
        w32(0x8000, 0x61000000u + (uint32_t)k);
    gc_write8(&sys.mem, IO + 0x8000, 0x61);
    CHECK_EQ_U64(sys.gx.fifo_bytes, 33);
    teardown();
}

const test_case_t hw_io_tests[] = {
    { "si: estado, origen y lectura directa por el búfer; puerto vacío", test_si_commands },
    { "si: sondeo de la VI, RDST, NOREP e interrupción", test_si_poll },
    { "exi: reloj del IPL, SRAM por DMA con sus sumas, escritura", test_exi_rtc_sram },
    { "exi: UART al log, TCINT por el PI, ranuras vacías, EXTINT", test_exi_uart_irq_slots },
    { "dsp: correos del ROM e INIT, reset, DSPInitCode", test_dsp_mail },
    { "dsp: DMA de la ARAM en los dos sentidos e interrupción", test_dsp_aram_dma },
    { "dsp: DMA de audio por bloques y AID", test_dsp_audio_dma },
    { "ai: contador de muestras, AIIT, parar y reiniciar", test_ai },
    { "di: sin disco, Inquiry, lectura con error y RequestError", test_di },
    { "mi, cp, pe y FIFO de GX: registros guardados y bytes contados", test_mi_gx },
    { NULL, NULL },
};
