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

/* Processor Interface y Video Interface: registros, tiempos e interrupciones. */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "dol_build.h"
#include "test.h"

#define TEXT0   0x80003100u
#define PI_BASE 0xCC003000u
#define VI_BASE 0xCC002000u

/* NTSC 480i como lo programa el SDK: ACV 240, EQU 6, PRB 24/25, PSB 3/2, 27 MHz */
#define VTR_480I 0x0F06u
#define VTO_480I 0x00030018u
#define VTE_480I 0x00020019u
#define HALF_LINE_480I 15444u            /* 36 ciclos por muestra x HLW 429 */
#define HALF_LINE_54   7722u             /* el valor de encendido de NTSC: 54 MHz */

static gc_system_t sys;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

/* li r3,0; bucle: addi r3,r3,1; b bucle */
static const uint32_t spin[] = { 0x38600000u, 0x38630001u, 0x4BFFFFFCu };

static int boot_code(const uint32_t *code, uint32_t n, int pal)
{
    static dolb_t b;
    const gc_boot_config_t cfg = { pal, 0 };
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    dolb_init(&b);
    dolb_code(&b, 0, TEXT0, code, n);
    dolb_entry(&b, TEXT0);
    return gc_system_boot_dol(&sys, b.buf, b.len, &cfg) == GC_DOL_OK ? 0 : -1;
}

static int boot(int pal) { return boot_code(spin, 3, pal); }

static void teardown(void)
{
    gc_system_free(&sys);
    gc_log_set_sink(NULL);
}

static uint32_t pi32(uint32_t off) { return gc_read32(&sys.mem, PI_BASE + off); }
static void wpi32(uint32_t off, uint32_t v) { gc_write32(&sys.mem, PI_BASE + off, v); }
static uint32_t vi16(uint32_t off) { return gc_read16(&sys.mem, VI_BASE + off); }
static uint32_t vi32(uint32_t off) { return gc_read32(&sys.mem, VI_BASE + off); }
static void wvi16(uint32_t off, uint32_t v) { gc_write16(&sys.mem, VI_BASE + off, (uint16_t)v); }
static void wvi32(uint32_t off, uint32_t v) { gc_write32(&sys.mem, VI_BASE + off, v); }

/* Corre hasta el ciclo `t` sin atender los eventos de ese ciclo. */
static void run_to(uint64_t t)
{
    if (t > sys.cpu.cycles)
        gc_system_run(&sys, t - sys.cpu.cycles);
}

static void setup_480i(void)
{
    wvi16(VI_VTR, VTR_480I);
    wvi32(VI_VTO, VTO_480I);
    wvi32(VI_VTE, VTE_480I);
    wvi16(VI_CLOCK, 0);
}

/* El evento que procesa la media línea h (y pasa a h+1) cae en este ciclo:
 * el primero quedó en HALF_LINE_54 (agendado al encender) y desde ahí cada
 * HALF_LINE_480I. */
static uint64_t at_half_line(uint32_t h) { return HALF_LINE_54 + (uint64_t)h * HALF_LINE_480I; }

/* ---- PI ------------------------------------------------------------------------ */

static void test_pi_boot(void)
{
    CHECK(boot(0) == 0);
    CHECK_EQ_U64(pi32(PI_INTSR), 0x00010100u);           /* reset suelto + VI */
    CHECK_EQ_U64(pi32(PI_INTMR), 0);
    CHECK_EQ_U64(pi32(PI_FLIPPER_REV), 0x246500B1u);
    CHECK_EQ_U64(pi32(PI_UNKNOWN), 0x1FFu);
    CHECK_EQ_U64(pi32(PI_BUS_STRENGTH), 0x02492492u);
    CHECK_EQ_U64(gc_read16(&sys.mem, PI_BASE + 0), 0x0001u);      /* lecturas de 16: mitades */
    CHECK_EQ_U64(gc_read16(&sys.mem, PI_BASE + 2), 0x0100u);
    CHECK(sys.cpu.external == 0);
    teardown();
}

static void test_pi_cause_mask(void)
{
    CHECK(boot(0) == 0);
    wpi32(PI_INTSR, PI_INT_VI);                              /* 1 borra */
    CHECK_EQ_U64(pi32(PI_INTSR), PI_RSW_STATE);
    wpi32(PI_INTSR, 0xFFFFFFFFu & ~PI_RSW_STATE);            /* escribir 1 no prende */
    CHECK_EQ_U64(pi32(PI_INTSR), PI_RSW_STATE);

    gc_pi_set_interrupt(&sys.pi, PI_INT_SI, 1);
    CHECK(sys.cpu.external == 0);                            /* sin máscara */
    wpi32(PI_INTMR, PI_INT_SI);
    CHECK(sys.cpu.external == 1);
    gc_write16(&sys.mem, PI_BASE + PI_INTMR, 0);             /* 16 bits: no existe */
    CHECK_EQ_U64(pi32(PI_INTMR), PI_INT_SI);
    gc_pi_set_interrupt(&sys.pi, PI_INT_SI, 0);
    CHECK(sys.cpu.external == 0);
    gc_pi_set_interrupt(&sys.pi, PI_INT_SI, 1);
    wpi32(PI_INTSR, PI_INT_SI);
    CHECK(sys.cpu.external == 0);

    wpi32(PI_FIFO_BASE, 0x80123457u);                        /* física, alineada a 32 */
    CHECK_EQ_U64(pi32(PI_FIFO_BASE), 0x00123440u);
    wpi32(PI_FLIPPER_REV, 0);                                /* solo lectura */
    CHECK_EQ_U64(pi32(PI_FLIPPER_REV), 0x246500B1u);
    teardown();
}

/* La línea del PI llega a la CPU: el manejador en 0x500 cuenta y apaga la máscara. */
static void test_pi_external_exception(void)
{
    static const uint32_t code[] = {
        0x3BC00000u,    /* li r30,0 */
        0x3BE00000u,    /* li r31,0 */
        0x7C8000A6u,    /* mfmsr r4 */
        0x60848000u,    /* ori r4,r4,0x8000 (EE) */
        0x7C800124u,    /* mtmsr r4 */
        0x3BBD0001u,    /* bucle: addi r29,r29,1 */
        0x4BFFFFFCu,    /* b bucle */
    };
    static const uint32_t handler[] = {
        0x3BFF0001u,    /* addi r31,r31,1 */
        0x3C60CC00u,    /* lis r3,0xCC00 */
        0x93C33004u,    /* stw r30,0x3004(r3): INTMR = 0 */
        0x4C000064u,    /* rfi */
    };
    CHECK(boot_code(code, 7, 0) == 0);
    for (uint32_t k = 0; k < 4; k++)
        gc_write32(&sys.mem, 0x80000500u + 4 * k, handler[k]);
    gc_system_run(&sys, 10);
    CHECK(sys.cpu.msr & MSR_EE);
    gc_pi_set_interrupt(&sys.pi, PI_INT_DSP, 1);
    wpi32(PI_INTMR, PI_INT_DSP);
    gc_system_run(&sys, 100);
    CHECK_EQ_U64(sys.cpu.gpr[31], 1);
    CHECK_EQ_U64(pi32(PI_INTMR), 0);
    CHECK(sys.cpu.spr[SPR_SRR0] == TEXT0 + 0x14 || sys.cpu.spr[SPR_SRR0] == TEXT0 + 0x18);
    CHECK(sys.cpu.msr & MSR_EE);                                  /* rfi la devolvió */
    CHECK(sys.cpu.pc == TEXT0 + 0x14 || sys.cpu.pc == TEXT0 + 0x18);
    CHECK(sys.cpu.gpr[29] > 50);
    teardown();
}

/* ---- VI ------------------------------------------------------------------------ */

static void test_vi_preset(void)
{
    CHECK(boot(0) == 0);
    CHECK_EQ_U64(vi16(VI_VTR), 0x0006u);
    CHECK_EQ_U64(vi16(VI_DCR), 0x0001u);
    CHECK_EQ_U64(vi32(VI_HTR0), 0x476901ADu);
    CHECK_EQ_U64(vi32(VI_HTR1), 0x02EA5140u);
    CHECK_EQ_U64(vi32(VI_VTO), 0x000501F6u);
    CHECK_EQ_U64(vi32(VI_VTE), 0x000401F7u);
    CHECK_EQ_U64(vi32(VI_BBOI), 0x410C410Cu);
    CHECK_EQ_U64(vi32(VI_BBEI), 0x40ED40EDu);
    CHECK_EQ_U64(vi32(VI_DI0), 0x110701AEu);
    CHECK_EQ_U64(vi32(VI_DI1), 0x10010001u);
    CHECK_EQ_U64(vi32(VI_DI2), 0);
    CHECK_EQ_U64(vi16(VI_HSW), 0x2828u);
    CHECK_EQ_U64(vi16(VI_CLOCK), 1);
    CHECK_EQ_U64(vi32(VI_TFBL), 0);
    CHECK_EQ_U64(vi16(VI_DPV), 1);
    CHECK_EQ_U64(vi16(VI_DPH), 1);
    CHECK_EQ_U64(gc_read8(&sys.mem, VI_BASE + VI_HTR0), 0x47u);    /* 8 bits: del registro de 16 */
    CHECK_EQ_U64(gc_read8(&sys.mem, VI_BASE + VI_HTR0 + 3), 0xADu);
    CHECK_EQ_U64(gc_vi_half_lines_odd(&sys.vi), 525);
    CHECK_EQ_U64(gc_vi_half_lines_even(&sys.vi), 525);
    CHECK_EQ_U64(gc_vi_ticks_per_half_line(&sys.vi), HALF_LINE_54);
    teardown();

    CHECK(boot(1) == 0);                                     /* PAL: FMT 1 y 27 MHz */
    CHECK_EQ_U64(vi16(VI_DCR), 0x0101u);
    CHECK_EQ_U64(vi16(VI_CLOCK), 0);
    CHECK_EQ_U64(gc_vi_ticks_per_half_line(&sys.vi), HALF_LINE_480I);
    teardown();
}

/* Como Dolphin, la causa VI del PI sigue prendida hasta la primera media línea. */
static void test_vi_first_half_line(void)
{
    CHECK(boot(0) == 0);
    run_to(HALF_LINE_54);
    CHECK(pi32(PI_INTSR) & PI_INT_VI);
    CHECK_EQ_U64(vi16(VI_DPV), 1);
    gc_system_run(&sys, 1);
    CHECK(!(pi32(PI_INTSR) & PI_INT_VI));
    CHECK_EQ_U64(vi16(VI_DPV), 1);                           /* media línea 1: sigue en la línea 1 */
    run_to(2 * HALF_LINE_54 + 1);
    CHECK_EQ_U64(vi16(VI_DPV), 2);
    teardown();
}

static void test_vi_480i_timing(void)
{
    CHECK(boot(0) == 0);
    setup_480i();
    CHECK_EQ_U64(gc_vi_half_lines_odd(&sys.vi), 525);
    CHECK_EQ_U64(gc_vi_half_lines_even(&sys.vi), 525);
    CHECK_EQ_U64(gc_vi_ticks_per_half_line(&sys.vi), HALF_LINE_480I);
    /* 525 medias líneas x 15 444 ciclos = 8 108 100 por campo: 59,94 Hz y
     * 675 675 ticks de timebase */
    CHECK_EQ_U64(525ull * HALF_LINE_480I, 8108100u);
    CHECK_EQ_U64(GC_CPU_CLOCK / 8108100.0 * 1000, 59940);

    run_to(at_half_line(99) + 1);                            /* ya en la media línea 100 */
    CHECK_EQ_U64(vi16(VI_DPV), 51);
    CHECK_EQ_U64(vi16(VI_DPH), 1);
    run_to(at_half_line(99) + HALF_LINE_480I / 2);           /* mitad de la primera media línea */
    CHECK_EQ_U64(vi16(VI_DPH), 1 + 429 * (HALF_LINE_480I / 2) / HALF_LINE_480I);
    run_to(at_half_line(100) + 1);                           /* 101: segunda mitad de la línea */
    CHECK_EQ_U64(vi16(VI_DPV), 51);
    CHECK(vi16(VI_DPH) > 429);
    run_to(at_half_line(1049) + 1);                          /* vuelta al principio del cuadro */
    CHECK_EQ_U64(vi16(VI_DPV), 1);
    CHECK_EQ_U64(sys.vi.half_line, 0);
    CHECK_EQ_U64(sys.vi.fields, 2);
    teardown();
}

/* DI0 (línea 263, segunda mitad) y DI1 (línea 1) del encendido: una vez por campo en 480i. */
static void test_vi_display_interrupts(void)
{
    CHECK(boot(0) == 0);
    setup_480i();
    wpi32(PI_INTMR, PI_INT_VI);
    run_to(at_half_line(0) + 1);
    CHECK(!(pi32(PI_INTSR) & PI_INT_VI));
    CHECK(sys.cpu.external == 0);

    run_to(at_half_line(524));                               /* justo antes */
    CHECK(!(vi32(VI_DI0) & VI_DI_INT));
    gc_system_run(&sys, 1);                                  /* media línea 525 */
    CHECK_EQ_U64(vi32(VI_DI0), 0x910701AEu);
    CHECK(pi32(PI_INTSR) & PI_INT_VI);
    CHECK(sys.cpu.external == 1);
    CHECK(!(vi32(VI_DI1) & VI_DI_INT));

    wvi16(VI_DI0, 0x1107);                                   /* borra INT, deja la máscara */
    CHECK_EQ_U64(vi32(VI_DI0), 0x110701AEu);
    CHECK(!(pi32(PI_INTSR) & PI_INT_VI));
    CHECK(sys.cpu.external == 0);

    run_to(at_half_line(1049) + 1);                          /* media línea 0: DI1 */
    CHECK_EQ_U64(vi32(VI_DI1), 0x90010001u);
    CHECK(sys.cpu.external == 1);
    wvi16(VI_DI1, 0x8001);                                   /* activa sin máscara: no avisa */
    CHECK(sys.cpu.external == 0);
    wvi16(VI_DI1, 0x9001);                                   /* el programa puede prenderla */
    CHECK(sys.cpu.external == 1);
    wvi16(VI_DI1, 0x1001);
    CHECK(sys.cpu.external == 0);
    teardown();
}

/* El reset de DCR borra las interrupciones y no queda escrito. */
static void test_vi_dcr_reset(void)
{
    CHECK(boot(0) == 0);
    setup_480i();
    run_to(at_half_line(524) + 1);
    CHECK(pi32(PI_INTSR) & PI_INT_VI);
    wvi16(VI_DCR, 0x0003);                                   /* ENB + RST */
    CHECK_EQ_U64(vi16(VI_DCR), 0x0001u);
    CHECK_EQ_U64(vi32(VI_DI0), 0);
    CHECK_EQ_U64(vi32(VI_DI1), 0);
    CHECK(!(pi32(PI_INTSR) & PI_INT_VI));
    teardown();
}

static gc_vi_field_t got[4];
static int ngot;

static void on_field(void *ctx, const gc_vi_field_t *f)
{
    (void)ctx;
    if (ngot < 4)
        got[ngot] = *f;
    ngot++;
}

/* Fin de cada campo: el XFB de arriba en el impar y el de abajo en el par. */
static void test_vi_fields(void)
{
    CHECK(boot(0) == 0);
    setup_480i();
    sys.vi.on_field = on_field;
    ngot = 0;
    memset(got, 0, sizeof got);
    wvi32(VI_TFBL, 0x10008000u);                             /* POFF: 0x00100000 >> 5 */
    wvi32(VI_BFBL, 0x10008028u);                             /* una línea de 640 más abajo */
    wvi16(VI_HSW, 0x2850);                                   /* WPL 40, STD 80 */

    run_to(at_half_line(521));                               /* última media línea del impar */
    CHECK(ngot == 0);
    gc_system_run(&sys, 1);
    CHECK(ngot == 1);
    CHECK_EQ_U64(got[0].xfb, 0x00100000u);
    CHECK_EQ_U64(got[0].width, 640);
    CHECK_EQ_U64(got[0].stride, 2560);
    CHECK_EQ_U64(got[0].height, 240);
    CHECK(got[0].even == 0);
    run_to(at_half_line(1046) + 1);
    CHECK(ngot == 2);
    CHECK_EQ_U64(got[1].xfb, 0x00100500u);
    CHECK(got[1].even == 1);
    CHECK_EQ_U64(sys.vi.fields, 2);
    CHECK(memcmp(&sys.vi.last, &got[1], sizeof got[1]) == 0);
    CHECK(memcmp(&sys.vi.field[0], &got[0], sizeof got[0]) == 0);   /* el último de cada paridad */
    CHECK(memcmp(&sys.vi.field[1], &got[1], sizeof got[1]) == 0);

    /* Sin POFF la dirección va tal cual; CLRPOFF apaga POFF */
    wvi32(VI_TFBL, 0x00200000u);
    wvi32(VI_BFBL, 0x00200500u);
    run_to(at_half_line(1050 + 521) + 1);
    CHECK(ngot == 3);
    CHECK_EQ_U64(got[2].xfb, 0x00200000u);
    wvi32(VI_TFBL, 0x10008000u);
    wvi16(VI_TFBL, 0x3000);                                  /* POFF + CLRPOFF */
    CHECK_EQ_U64(vi16(VI_TFBL), 0x2000u);
    teardown();

    /* Sin XFB se cuenta el campo pero no se informa */
    CHECK(boot(0) == 0);
    setup_480i();
    sys.vi.on_field = on_field;
    ngot = 0;
    run_to(at_half_line(1046) + 1);
    CHECK(ngot == 0);
    CHECK_EQ_U64(sys.vi.fields, 2);
    teardown();
}

static void test_vi_bad_access(void)
{
    CHECK(boot(0) == 0);
    gc_write8(&sys.mem, VI_BASE + VI_HSW, 0x12);             /* 8 bits: no existe */
    CHECK_EQ_U64(vi16(VI_HSW), 0x2828u);
    wvi16(VI_DPV, 77);                                       /* posición del haz: solo lectura */
    CHECK_EQ_U64(vi16(VI_DPV), 1);
    wvi16(0x80, 0x1234);                                     /* fuera de los registros */
    CHECK_EQ_U64(vi16(0x80), 0);
    CHECK_EQ_U64(vi16(VI_VTR), 0x0006u);
    teardown();
}

const test_case_t hw_tests[] = {
    { "pi: estado de encendido, revisión y lecturas de 16 bits", test_pi_boot },
    { "pi: INTSR se borra con 1, INTMR maneja la línea externa", test_pi_cause_mask },
    { "pi: interrupción externa a la CPU (manejador en 0x500)", test_pi_external_exception },
    { "vi: valores de encendido NTSC y PAL", test_vi_preset },
    { "vi: la causa VI del encendido dura hasta la primera media línea", test_vi_first_half_line },
    { "vi: 480i NTSC, medias líneas, DPV y DPH", test_vi_480i_timing },
    { "vi: DI0 y DI1 por campo, borrado y máscara", test_vi_display_interrupts },
    { "vi: reset de DCR", test_vi_dcr_reset },
    { "vi: fin de campo con el XFB, POFF y CLRPOFF", test_vi_fields },
    { "vi: accesos de 8 bits, haz y fuera de rango", test_vi_bad_access },
    { NULL, NULL },
};
