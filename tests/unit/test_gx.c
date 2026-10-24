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
 * Fase 2, etapa A: el gather pipe, el FIFO de GX (CP y PI), el tiempo de la
 * GPU (SyncGPU cada 1000 ciclos con presupuesto y deuda), el lector de
 * comandos, el estado de BP, CP y XF y el Pixel Engine. Los valores esperados
 * salen de leer el código de Dolphin cf5cb7ef; la comparación de punta a punta
 * es el DOL gx_fifo contra la traza de Dolphin (test_dolphin.c).
 */
#include <string.h>

#include "core/log.h"
#include "core/system.h"
#include "dol_build.h"
#include "test.h"

#define TEXT0 0x80003100u
#define IO    0xCC000000u
#define PIPE  0x8000u
#define BASE  0x00100000u                     /* el FIFO de las pruebas, físico */

static gc_system_t sys;

static void quiet(char level, const char *msg)
{
    (void)level;
    (void)msg;
}

/* li r3,0; bucle: addi r3,r3,1; b bucle */
static const uint32_t spin[] = { 0x38600000u, 0x38630001u, 0x4BFFFFFCu };

static int boot_code(const uint32_t *code, uint32_t n)
{
    static dolb_t b;
    const gc_boot_config_t cfg = { 0, 100 };
    gc_log_set_sink(quiet);
    if (gc_system_init(&sys) != 0)
        return -1;
    dolb_init(&b);
    dolb_code(&b, 0, TEXT0, code, n);
    dolb_entry(&b, TEXT0);
    return gc_system_boot_dol(&sys, b.buf, b.len, &cfg) == GC_DOL_OK ? 0 : -1;
}

static int boot(void)
{
    return boot_code(spin, 3);
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
static uint64_t now(void) { return sys.cpu.cycles; }

static void run_to(uint64_t t)
{
    if (t > sys.cpu.cycles)
        gc_system_run(&sys, t - sys.cpu.cycles);
}

/* Un par LO/HI del CP */
static void cp_set(uint32_t lo, uint32_t v)
{
    w16(lo, v & 0xFFFFu);
    w16(lo + 2, v >> 16);
}

static uint32_t cp_get(uint32_t lo)
{
    return r16(lo) | r16(lo + 2) << 16;
}

/* FIFO de `size` bytes en BASE, enlazado, con la marca alta en `hi` */
static void fifo_setup(uint32_t size, uint32_t hi, uint32_t lo, uint16_t ctrl)
{
    w16(CP_CR, 0);
    cp_set(CP_FIFO_BASE_LO, BASE);
    cp_set(CP_FIFO_END_LO, BASE + size - 32);
    cp_set(CP_FIFO_HI_WM_LO, hi);
    cp_set(CP_FIFO_LO_WM_LO, lo);
    cp_set(CP_FIFO_DIST_LO, 0);
    cp_set(CP_FIFO_WPTR_LO, BASE);
    cp_set(CP_FIFO_RPTR_LO, BASE);
    w32(0x3000 + PI_FIFO_BASE, BASE);
    w32(0x3000 + PI_FIFO_END, BASE + size - 32);
    w32(0x3000 + PI_FIFO_WPTR, BASE);
    w16(CP_CR, ctrl);
}

static void gp8(uint32_t v) { gc_write8(&sys.mem, IO + PIPE, (uint8_t)v); }
static void gp16(uint32_t v) { gc_write16(&sys.mem, IO + PIPE, (uint16_t)v); }
static void gp32(uint32_t v) { gc_write32(&sys.mem, IO + PIPE, v); }

static void gp_bp(uint32_t reg, uint32_t v) { gp8(GX_LOAD_BP); gp32(reg << 24 | (v & 0xFFFFFFu)); }

/* NOPs hasta completar el bloque */
static void gp_pad(void)
{
    while (sys.gpfifo.count)
        gp8(0);
}

static void gp_nop_block(void)
{
    for (int k = 0; k < 8; k++)
        gp32(0);
}

/* ---- gather pipe y FIFO ------------------------------------------------------------ */

static void test_gather_pipe(void)
{
    CHECK(boot() == 0);
    /* Sin enlace: los bloques van a la RAM en el puntero del PI, nada más */
    w32(0x3000 + PI_FIFO_BASE, BASE);
    w32(0x3000 + PI_FIFO_END, BASE + 32);              /* dos bloques */
    w32(0x3000 + PI_FIFO_WPTR, BASE);
    for (uint32_t k = 0; k < 8; k++)
        gp32(0x01020304u + k);
    CHECK_EQ_U64(r32(0x3000 + PI_FIFO_WPTR), BASE + 32);
    CHECK_EQ_U64(gc_read32(&sys.mem, 0x80000000u | BASE), 0x01020304u);
    CHECK_EQ_U64(gc_read32(&sys.mem, 0x80000000u | (BASE + 28)), 0x0102030Bu);
    CHECK_EQ_U64(cp_get(CP_FIFO_DIST_LO), 0);

    gp16(0xAABB);                                      /* 16 y 8 bits, y la página entera */
    gc_write8(&sys.mem, IO + PIPE + 0xF00, 0xCC);
    CHECK_EQ_U64(sys.gpfifo.count, 3);
    for (int k = 0; k < 29; k++)
        gp8(0x11);
    CHECK_EQ_U64(gc_read32(&sys.mem, 0x80000000u | (BASE + 32)), 0xAABBCC11u);
    CHECK_EQ_U64(r32(0x3000 + PI_FIFO_WPTR), BASE);    /* después de END vuelve a la base */
    CHECK_EQ_U64(sys.gpfifo.count, 0);
    CHECK_EQ_U64(gc_read32(&sys.mem, IO + PIPE), 0);   /* no se lee */
    teardown();
}

static void test_cp_registers(void)
{
    CHECK(boot() == 0);
    CHECK_EQ_U64(r16(CP_SR), CP_SR_READ_IDLE | CP_SR_CMD_IDLE);
    w16(CP_FIFO_BASE_LO, 0xFFFF);                      /* LO: alineada a 32; HI: 10 bits */
    w16(CP_FIFO_BASE_HI, 0xFFFF);
    CHECK_EQ_U64(r32(CP_FIFO_BASE_LO), 0xFFE003FFu);   /* 32 bits: la mitad baja arriba */
    CHECK_EQ_U64(sys.cp.fifo.base, 0x03FFFFE0u);
    w32(CP_FIFO_END_LO, 0x12345678u);                  /* 0x1234 va a LO, 0x5678 a HI */
    CHECK_EQ_U64(sys.cp.fifo.end, 0x02781220u);
    w16(CP_CR, 0xFFFF);
    CHECK_EQ_U64(r16(CP_CR), 0x3F);
    CHECK(sys.cp.fifo.read_enable && sys.cp.fifo.bp_enable && sys.cp.fifo.link && sys.cp.fifo.bp_int);
    w16(CP_CR, 0);
    w16(CP_PERF_SELECT, 0xFF);
    CHECK_EQ_U64(r16(CP_PERF_SELECT), 7);
    w16(CP_UNK_0A, 0x1234);
    CHECK_EQ_U64(r16(CP_UNK_0A), 0x34);
    CHECK_EQ_U64(r16(CP_CLEAR), 0);
    CHECK_EQ_U64(r16(CP_CLKS_PER_VTX_OUT), 4);
    CHECK_EQ_U64(r16(0x50), 0);                        /* métricas */
    w16(CP_SR, 0xFFFF);                                /* no se escribe */
    CHECK_EQ_U64(r16(CP_SR), CP_SR_READ_IDLE | CP_SR_CMD_IDLE);
    teardown();
}

/* El bloque llega a la RAM y al CP en el acto; la GPU lo lee 1000 ciclos
 * después de que se despertó (aquí, con la escritura del control) */
static void test_fifo_sync_event(void)
{
    CHECK(boot() == 0);
    fifo_setup(0x1000, 0x1000, 0, CP_CR_READ | CP_CR_LINK);
    const uint64_t t0 = now();
    gp_bp(0x41, 0x00ABCD);
    gp_pad();
    CHECK_EQ_U64(sys.cp.fifo.distance, 32);
    CHECK_EQ_U64(sys.cp.fifo.wptr, BASE + 32);
    CHECK_EQ_U64(r32(0x3000 + PI_FIFO_WPTR), BASE + 32);
    run_to(t0 + GC_GX_SLOT);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE);              /* el evento es del ciclo t0 + 1000 */
    run_to(t0 + GC_GX_SLOT + 1);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 32);
    CHECK_EQ_U64(sys.cp.fifo.distance, 0);
    CHECK_EQ_U64(sys.gx.bp[0x41], 0x00ABCD);
    CHECK(sys.gx.suspended);                           /* 12 + 27 × 6 < 1000: ociosa */
    teardown();
}

/* 32 NOPs cuestan 192 ciclos: con 10 bloques, el primer turno lee 6 (queda
 * debiendo 152) y vuelve 1152 ciclos después con 1152 - 152 de presupuesto */
static void test_fifo_budget(void)
{
    CHECK(boot() == 0);
    fifo_setup(0x1000, 0x1000, 0, CP_CR_READ | CP_CR_LINK);
    const uint64_t t0 = now();
    for (int k = 0; k < 10; k++)
        gp_nop_block();
    run_to(t0 + GC_GX_SLOT + 1);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 6 * 32);
    CHECK_EQ_U64((uint32_t)sys.gx.sync_ticks, (uint32_t)-152);
    CHECK(!sys.gx.suspended);
    run_to(t0 + GC_GX_SLOT + 1152);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 6 * 32);
    run_to(t0 + GC_GX_SLOT + 1152 + 1);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 10 * 32);
    CHECK_EQ_U64(sys.gx.sync_ticks, 0);
    CHECK(sys.gx.suspended);
    teardown();
}

/* Leer SR o la mitad alta de la distancia corre la GPU en el acto; la mitad
 * baja, no */
static void test_register_sync(void)
{
    CHECK(boot() == 0);
    fifo_setup(0x1000, 0x1000, 0, CP_CR_READ | CP_CR_LINK);
    gp_nop_block();
    CHECK_EQ_U64(r16(CP_FIFO_DIST_LO), 32);
    CHECK_EQ_U64(r16(CP_FIFO_RPTR_LO), BASE & 0xFFFF);
    CHECK_EQ_U64(r16(CP_FIFO_DIST_HI), 0);
    CHECK_EQ_U64(r16(CP_FIFO_DIST_LO), 0);
    gp_nop_block();
    CHECK_EQ_U64(r16(CP_SR), CP_SR_READ_IDLE | CP_SR_CMD_IDLE);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 64);
    gp_nop_block();
    CHECK_EQ_U64(r16(CP_FIFO_RPTR_HI), BASE >> 16);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 96);

    /* Sin leer: la GPU no avanza aunque la sincronicen */
    w16(CP_CR, CP_CR_LINK);
    gp_nop_block();
    CHECK_EQ_U64(r16(CP_SR), CP_SR_CMD_IDLE);          /* hay datos y nadie los lee */
    CHECK_EQ_U64(sys.cp.fifo.distance, 32);
    teardown();
}

/* El FIFO de dos bloques da la vuelta en la CPU, en el CP y en la GPU */
static void test_fifo_wrap(void)
{
    CHECK(boot() == 0);
    fifo_setup(64, 0x1000, 0, CP_CR_READ | CP_CR_LINK);
    gp_bp(0x41, 1);
    gp_pad();
    gp_bp(0x42, 2);
    gp_pad();
    CHECK_EQ_U64(sys.cp.fifo.wptr, BASE);
    CHECK_EQ_U64(r32(0x3000 + PI_FIFO_WPTR), BASE);
    CHECK_EQ_U64(r16(CP_SR) & CP_SR_READ_IDLE, CP_SR_READ_IDLE);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE);
    gp_bp(0x43, 3);
    gp_pad();
    CHECK_EQ_U64(sys.cp.fifo.wptr, BASE + 32);
    r16(CP_SR);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 32);
    CHECK_EQ_U64(sys.gx.bp[0x41], 1);
    CHECK_EQ_U64(sys.gx.bp[0x42], 2);
    CHECK_EQ_U64(sys.gx.bp[0x43], 3);
    teardown();
}

/* La marca alta se mira antes de sumar el bloque: con 64, salta en el cuarto */
static void test_watermarks(void)
{
    CHECK(boot() == 0);
    fifo_setup(0x1000, 64, 0, CP_CR_READ | CP_CR_LINK | CP_CR_OVF_INT);
    const uint64_t t0 = now();
    for (int k = 0; k < 3; k++)
        gp_nop_block();
    CHECK(!pi_cause(PI_INT_CP));
    gp_nop_block();
    CHECK(pi_cause(PI_INT_CP));
    run_to(t0 + GC_GX_SLOT + 1);                       /* la GPU la vacía: se baja sola */
    CHECK_EQ_U64(sys.cp.fifo.distance, 0);
    CHECK(!pi_cause(PI_INT_CP));

    /* La baja: 4 bloques sin interrupciones, después se habilita y la GPU la cruza */
    w16(CP_CR, CP_CR_READ | CP_CR_LINK);
    const uint64_t t1 = now();
    for (int k = 0; k < 4; k++)
        gp_nop_block();
    cp_set(CP_FIFO_LO_WM_LO, 64);
    w16(CP_CR, CP_CR_READ | CP_CR_LINK | CP_CR_UNDF_INT);
    CHECK(!pi_cause(PI_INT_CP));
    run_to(t1 + GC_GX_SLOT + 1);
    CHECK(pi_cause(PI_INT_CP));
    CHECK_EQ_U64(r16(CP_SR), CP_SR_UNDERFLOW | CP_SR_READ_IDLE | CP_SR_CMD_IDLE);
    w16(CP_CR, CP_CR_READ | CP_CR_LINK);               /* se apaga en la próxima revisión */
    gp_nop_block();
    CHECK(!pi_cause(PI_INT_CP));
    teardown();
}

/* El breakpoint detiene la lectura con su interrupción; sin BP_ENABLE sigue */
static void test_breakpoint(void)
{
    CHECK(boot() == 0);
    fifo_setup(0x1000, 0x1000, 0, CP_CR_READ | CP_CR_LINK | CP_CR_BP_ENABLE | CP_CR_BP_INT);
    cp_set(CP_FIFO_BP_LO, BASE + 32);
    const uint64_t t0 = now();
    gp_nop_block();
    gp_bp(0x41, 0x55);
    gp_pad();
    gp_nop_block();
    run_to(t0 + GC_GX_SLOT + 1);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 32);
    CHECK(pi_cause(PI_INT_CP));
    CHECK(sys.gx.suspended);
    CHECK_EQ_U64(r16(CP_SR), CP_SR_BREAKPOINT | CP_SR_CMD_IDLE);
    CHECK_EQ_U64(sys.gx.bp[0x41], 0);

    w16(CP_CR, CP_CR_READ | CP_CR_LINK);
    const uint64_t t1 = now();
    run_to(t1 + GC_GX_SLOT + 1);
    CHECK_EQ_U64(sys.cp.fifo.rptr, BASE + 96);
    CHECK_EQ_U64(sys.gx.bp[0x41], 0x55);
    CHECK(!pi_cause(PI_INT_CP));
    CHECK_EQ_U64(r16(CP_SR), CP_SR_READ_IDLE | CP_SR_CMD_IDLE);
    teardown();
}

/* PI_FIFO_RESET: gather pipe vacío, CP enlazado sin leer, marcas por omisión */
static void test_pi_fifo_reset(void)
{
    CHECK(boot() == 0);
    fifo_setup(0x1000, 64, 32, CP_CR_READ | CP_CR_LINK | CP_CR_OVF_INT);
    gp8(0x61);
    w32(0x3000 + PI_FIFO_RESET, 1);
    CHECK_EQ_U64(sys.gpfifo.count, 0);
    CHECK_EQ_U64(r16(CP_CR), CP_CR_LINK);
    CHECK_EQ_U64(cp_get(CP_FIFO_HI_WM_LO), 0x03FFFFE0u);
    CHECK_EQ_U64(cp_get(CP_FIFO_LO_WM_LO), 0);
    w32(0x3000 + PI_FIFO_RESET, 0);
    teardown();
}

/* Un evento a pocos ciclos que agenda una escritura de la CPU corta su tramo.
 * Sin otros eventos, gc_system_run corre todo en un tramo: el token solo
 * aparece si la GPU leyó a los 1000 ciclos del último stw y el PE levantó la
 * señal 500 después, dentro de la misma llamada. */
static void test_event_cuts_slice(void)
{
    static const uint32_t code[] = {
        0x3C60CC00u,                                   /* lis r3, 0xCC00 */
        0x60638000u,                                   /* ori r3, r3, 0x8000 */
        0x3C806147u,                                   /* lis r4, 0x6147   BP 0x47 (token) */
        0x60840012u,                                   /* ori r4, r4, 0x0012 */
        0x90830000u,                                   /* stw r4, 0(r3) */
        0x3C803400u,                                   /* lis r4, 0x3400   token 0x1234 */
        0x90830000u,
        0x38800000u,                                   /* li r4, 0         NOPs */
        0x90830000u, 0x90830000u, 0x90830000u, 0x90830000u, 0x90830000u, 0x90830000u,
        0x48000000u,                                   /* b . */
    };
    CHECK(boot_code(code, sizeof code / sizeof code[0]) == 0);
    fifo_setup(0x1000, 0x1000, 0, CP_CR_READ | CP_CR_LINK);
    gc_sched_init(&sys.sched);                         /* sin la VI ni el evento del control */
    sys.sched.limit = &sys.cpu.run_end;
    sys.gx.suspended = 1;
    const uint64_t last_stw = GC_BOOT_CYCLES + 13;
    run_to(last_stw + GC_GX_SLOT + GC_PE_MIN_DELAY + 1);
    CHECK_EQ_U64(r16(0x1000 + PE_TOKEN), 0x1234);
    teardown();
}

/* ---- lector de comandos ----------------------------------------------------------- */

static uint32_t run_bytes(const uint8_t *p, uint32_t n)
{
    sys.gx.cycles = 0;
    return gc_gx_run_commands(&sys.gx, p, n);
}

static void test_commands(void)
{
    CHECK(boot() == 0);
    static const uint8_t cmds[] = {
        0x08, 0x50, 0x00, 0x00, 0x02, 0x00,            /* VCD_LO: posición directa */
        0x08, 0x31, 0x12, 0x34, 0x56, 0x78,            /* 0x31 también es MATINDEX_A */
        0x08, 0x78, 0x00, 0x00, 0x00, 0x09,            /* VAT A del formato 0 */
        0x08, 0xA3, 0xFF, 0xFF, 0xFF, 0xFF,            /* base del arreglo 3: 26 bits */
        0x08, 0xB3, 0x00, 0x00, 0x01, 0xFF,            /* paso: 8 bits */
        0x08, 0x20, 0x00, 0x00, 0x00, 0x00,            /* sin efecto */
        0x00, 0x00, 0x00,                              /* 3 NOPs: un comando */
        0x09,                                          /* desconocido: 1 byte, 1 ciclo */
        0x44, 0x48,                                    /* 6 ciclos cada uno */
        0x61, 0xFE, 0x00, 0x00, 0xFF,                  /* máscara de BP */
        0x61, 0x41, 0x12, 0x34, 0x56,                  /* solo pasa 0x56 */
        0x61, 0x42, 0xAB, 0xCD, 0xEF,                  /* la máscara ya volvió */
        0x10, 0x00, 0x01, 0x0F, 0xFE,                  /* XF: 2 palabras en 0x0FFE */
        0x3F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11,
    };
    const uint32_t used = run_bytes(cmds, sizeof cmds);
    CHECK_EQ_U64(used, sizeof cmds);
    CHECK_EQ_U64(sys.gx.cycles, 6 * 12 + 3 * 6 + 1 + 6 + 6 + 3 * 12 + 18 + 2 * 6);
    CHECK_EQ_U64(sys.gx.cp.vcd_lo, 0x200);
    CHECK_EQ_U64(sys.gx.cp.matindex_a, 0x12345678u);
    CHECK_EQ_U64(sys.gx.cp.vat[0][0], 9);
    CHECK_EQ_U64(sys.gx.cp.array_base[3], 0x03FFFFFFu);
    CHECK_EQ_U64(sys.gx.cp.array_stride[3], 0xFF);
    CHECK_EQ_U64(sys.gx.bp[0x41], 0x56);
    CHECK_EQ_U64(sys.gx.bp[0x42], 0xABCDEF);
    CHECK_EQ_U64(sys.gx.bp[GX_BP_MASK], 0xFFFFFF);
    CHECK_EQ_U64(sys.gx.xf[0x0FFE], 0x3F800000u);
    CHECK_EQ_U64(sys.gx.xf[0x0FFF], 0x11);
    CHECK_EQ_U64(sys.gx.unknown, 1);

    uint32_t cpmem[256];
    gc_gx_cp_memory(&sys.gx, cpmem);
    CHECK_EQ_U64(cpmem[0x30], 0x12345678u);
    CHECK_EQ_U64(cpmem[0x70], 9);
    CHECK_EQ_U64(cpmem[0xA3], 0x03FFFFFFu);
    CHECK_EQ_U64(cpmem[0xB3], 0xFF);

    /* Incompletos: no consumen nada */
    CHECK_EQ_U64(run_bytes(cmds, 5), 0);               /* CP necesita 6 */
    static const uint8_t xf_part[] = { 0x10, 0x00, 0x02, 0x10, 0x00, 0, 0, 0, 0 };
    CHECK_EQ_U64(run_bytes(xf_part, sizeof xf_part), 0);   /* 3 palabras: 17 bytes */
    static const uint8_t two[] = { 0x61, 0x43, 0, 0, 1, 0x61, 0x44, 0 };
    CHECK_EQ_U64(run_bytes(two, sizeof two), 5);
    teardown();
}

/* XF: memoria y registros, el cruce en 0x1000 y el corte en el último registro */
static void test_xf_loads(void)
{
    CHECK(boot() == 0);
    uint8_t w[16 * 4];
    for (int k = 0; k < 16; k++)
        dolb_put32(w + 4 * k, 0xA0000000u + (uint32_t)k);
    gc_gx_load_xf(&sys.gx, 0x0FFE, 4, w);
    CHECK_EQ_U64(sys.gx.xf[0x0FFE], 0xA0000000u);
    CHECK_EQ_U64(sys.gx.xf[0x0FFF], 0xA0000001u);
    CHECK_EQ_U64(sys.gx.xf[0x1000], 0xA0000002u);
    CHECK_EQ_U64(sys.gx.xf[0x1001], 0xA0000003u);
    gc_gx_load_xf(&sys.gx, 0x1056, 4, w);
    CHECK_EQ_U64(sys.gx.xf[0x1056], 0xA0000000u);
    CHECK_EQ_U64(sys.gx.xf[0x1057], 0xA0000001u);
    gc_gx_load_xf(&sys.gx, 0x1058, 4, w);              /* justo al final: nada */
    gc_gx_load_xf(&sys.gx, 0x2000, 4, w);
    CHECK_EQ_U64(sys.gx.xf[0x1057], 0xA0000001u);

    /* Indexadas: matriz 2 de un arreglo de 48 bytes, luz 1 de uno de 64 */
    const uint32_t arr = 0x00200000u;
    for (uint32_t k = 0; k < 64; k++)
        gc_write32(&sys.mem, 0x80000000u | (arr + 4 * k), 0xB0000000u + k);
    sys.gx.cp.array_base[12] = arr;
    sys.gx.cp.array_stride[12] = 48;
    gc_gx_load_xf_indexed(&sys.gx, 12, 2, 0x000, 12);
    CHECK_EQ_U64(sys.gx.xf[0], 0xB0000000u + 24);
    CHECK_EQ_U64(sys.gx.xf[11], 0xB0000000u + 35);
    sys.gx.cp.array_base[15] = arr + 0x40;
    sys.gx.cp.array_stride[15] = 64;
    static const uint8_t indx_d[] = { 0x38, 0x00, 0x01, 0xF6, 0x10 };   /* índice 1, 16 palabras en 0x610 */
    CHECK_EQ_U64(run_bytes(indx_d, sizeof indx_d), 5);
    CHECK_EQ_U64(sys.gx.cycles, 6);
    CHECK_EQ_U64(sys.gx.xf[0x610], 0xB0000000u + 32);
    CHECK_EQ_U64(sys.gx.xf[0x61F], 0xB0000000u + 47);
    teardown();
}

static void test_vertex_size(void)
{
    CHECK(boot() == 0);
    gc_gx_cp_state_t *s = &sys.gx.cp;
    s->vcd_lo = 0x201;                                 /* PosMatIdx y posición directa */
    s->vat[0][0] = 0x9;                                /* XYZ float */
    CHECK_EQ_U64(gc_gx_vertex_size(&sys.gx, 0), 13);
    /* Tex3MatIdx, normal NTB con 3 índices de 16, color 0 RGBA8888, color 1
     * índice de 8, tex 0 ST u16, tex 7 índice de 16 */
    s->vcd_lo = 0x13A11;
    s->vcd_hi = 0xC001;
    s->vat[0][0] = 0x80A14E09u;
    CHECK_EQ_U64(gc_gx_vertex_size(&sys.gx, 0), 2 + 12 + 6 + 4 + 1 + 4 + 2);
    s->vat[0][0] &= 0x7FFFFFFFu;                       /* sin NormalIndex3: un índice */
    CHECK_EQ_U64(gc_gx_vertex_size(&sys.gx, 0), 2 + 12 + 2 + 4 + 1 + 4 + 2);
    s->vcd_lo = 0xA00;                                 /* posición directa, normal directa */
    s->vcd_hi = 0;
    s->vat[1][0] = 1u | 3u << 1 | 1u << 9 | 1u << 10;  /* XYZ s16, NTB s8 */
    CHECK_EQ_U64(gc_gx_vertex_size(&sys.gx, 1), 6 + 9);
    s->vcd_hi = 0x0004;                                /* tex 1 directa: g1 */
    s->vat[1][1] = 1u | 4u << 1;                       /* ST float */
    CHECK_EQ_U64(gc_gx_vertex_size(&sys.gx, 1), 6 + 9 + 8);
    teardown();
}

/* Las primitivas se saltan con su tamaño; lo que sigue se decodifica bien */
static void test_primitive_skip(void)
{
    CHECK(boot() == 0);
    sys.gx.cp.vcd_lo = 0x200;
    sys.gx.cp.vat[2][0] = 0x9;                         /* XYZ float: 12 bytes */
    uint8_t buf[64] = { 0x92, 0x00, 0x03 };            /* triángulos, formato 2, 3 vértices */
    for (int k = 0; k < 36; k++)
        buf[3 + k] = 0x61;                             /* datos que parecen comandos */
    const uint8_t bp[] = { 0x61, 0x50, 0x00, 0x00, 0x07 };
    memcpy(buf + 39, bp, sizeof bp);
    CHECK_EQ_U64(run_bytes(buf, 44), 44);
    CHECK_EQ_U64(sys.gx.cycles, 3 * 12 + 6 + 12);
    CHECK_EQ_U64(sys.gx.bp[0x50], 7);
    CHECK_EQ_U64(sys.gx.primitives, 1);
    CHECK_EQ_U64(sys.gx.vertices, 3);
    CHECK_EQ_U64(run_bytes(buf, 38), 0);               /* le falta un byte */
    const uint8_t empty[] = { 0xB8, 0x00, 0x00 };      /* puntos, 0 vértices */
    CHECK_EQ_U64(run_bytes(empty, 3), 3);
    CHECK_EQ_U64(sys.gx.cycles, 6);
    teardown();
}

/* Display list: sus ciclos cuentan, una dentro de otra se ignora, lo que no
 * entra al final se pierde y la dirección y el tamaño se alinean a 32 */
static void test_display_list(void)
{
    CHECK(boot() == 0);
    const uint32_t dl = 0x00300000u;
    static const uint8_t list[64] = {
        0x61, 0x60, 0x00, 0x00, 0x01,                  /* BP 0x60 = 1 */
        0x40, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,   /* otra lista: se ignora */
        0x61, 0x61, 0x00, 0x00, 0x02,                  /* BP 0x61 = 2 */
        [27] = 0x61, 0x62, 0x00, 0x00, 0x03,           /* BP 0x62 = 3, en los bytes 27-31 */
        [62] = 0x61, 0x63,                             /* incompleto al final */
    };
    uint8_t *ram = gc_ram_phys(&sys.mem, dl, sizeof list);
    CHECK(ram != NULL);
    memcpy(ram, list, sizeof list);
    const uint8_t call[] = { 0x40, 0x80, 0x30, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x5F };   /* 0x80300000+31, 95 */
    CHECK_EQ_U64(run_bytes(call, sizeof call), 9);
    CHECK_EQ_U64(sys.gx.bp[0x60], 1);
    CHECK_EQ_U64(sys.gx.bp[0x61], 2);
    CHECK_EQ_U64(sys.gx.bp[0x62], 3);
    CHECK_EQ_U64(sys.gx.bp[0x63], 0);
    CHECK_EQ_U64(sys.gx.display_lists, 1);
    /* 6 de la lista + 3 BP + la de adentro (6) + 8 NOPs (19-26) + 30 NOPs (32-61) */
    CHECK_EQ_U64(sys.gx.cycles, 6 + 3 * 12 + 6 + 8 * 6 + 30 * 6);
    teardown();
}

/* Un comando que cruza bloques espera al que lo completa */
static void test_command_across_blocks(void)
{
    CHECK(boot() == 0);
    fifo_setup(0x1000, 0x1000, 0, CP_CR_READ | CP_CR_LINK);
    gp8(GX_LOAD_XF);
    gp32(0x000F0100u);                                 /* 16 palabras en 0x100: 69 bytes */
    for (uint32_t k = 0; k < 16; k++)
        gp32(0xC0000000u + k);
    gp_pad();                                          /* 27 NOPs */
    CHECK_EQ_U64(sys.cp.fifo.distance, 96);
    run_to(now() + GC_GX_SLOT + 1);
    CHECK_EQ_U64(sys.cp.fifo.distance, 0);
    CHECK_EQ_U64(sys.gx.xf[0x100], 0xC0000000u);
    CHECK_EQ_U64(sys.gx.xf[0x10F], 0xC000000Fu);
    CHECK_EQ_U64(sys.gx.vrd, sys.gx.vwr);
    CHECK_EQ_U64(sys.gx.cycles, 18 + 16 * 6 + 27 * 6);     /* todo en el tercer bloque */
    teardown();
}

/* ---- Pixel Engine ---------------------------------------------------------------------- */

static void test_pe_token(void)
{
    CHECK(boot() == 0);
    fifo_setup(0x1000, 0x1000, 0, CP_CR_READ | CP_CR_LINK);
    w16(0x1000 + PE_CTRL, PE_CTRL_TOKEN_ENABLE);
    const uint64_t t0 = now();
    gp_bp(GX_BP_TOKEN_INT, 0xBEEF);
    gp_pad();
    run_to(t0 + GC_GX_SLOT + GC_PE_MIN_DELAY);         /* procesado en +1000, señal 500 después */
    CHECK_EQ_U64(r16(0x1000 + PE_TOKEN), 0);
    CHECK(!pi_cause(PI_INT_PE_TOKEN));
    run_to(t0 + GC_GX_SLOT + GC_PE_MIN_DELAY + 1);
    CHECK_EQ_U64(r16(0x1000 + PE_TOKEN), 0xBEEF);
    CHECK(pi_cause(PI_INT_PE_TOKEN));
    CHECK_EQ_U64(r16(0x1000 + PE_CTRL), PE_CTRL_TOKEN_ENABLE);
    w16(0x1000 + PE_CTRL, PE_CTRL_TOKEN_ENABLE | PE_CTRL_TOKEN);   /* se atiende */
    CHECK(!pi_cause(PI_INT_PE_TOKEN));
    CHECK_EQ_U64(r16(0x1000 + PE_CTRL), PE_CTRL_TOKEN_ENABLE);

    /* Dos en el mismo bloque: un solo evento, el token del segundo; sin
     * interrupción habilitada la señal queda y salta al habilitarla */
    w16(0x1000 + PE_CTRL, 0);
    const uint64_t t1 = now();
    gp_bp(GX_BP_TOKEN, 0x1111);
    gp_bp(GX_BP_TOKEN_INT, 0x2222);
    gp_pad();
    run_to(t1 + GC_GX_SLOT + GC_PE_MIN_DELAY + 1);
    CHECK_EQ_U64(r16(0x1000 + PE_TOKEN), 0x2222);
    CHECK(!pi_cause(PI_INT_PE_TOKEN));
    w16(0x1000 + PE_CTRL, PE_CTRL_TOKEN_ENABLE);
    CHECK(pi_cause(PI_INT_PE_TOKEN));
    teardown();
}

/* Draw done después de una display list larga: la señal espera los ciclos
 * acumulados (594 > 500) */
static void test_pe_finish(void)
{
    CHECK(boot() == 0);
    uint8_t *ram = gc_ram_phys(&sys.mem, 0x00300000u, 96);
    memset(ram, 0, 96);                                /* 96 NOPs: 576 ciclos */
    fifo_setup(0x1000, 0x1000, 0, CP_CR_READ | CP_CR_LINK);
    w16(0x1000 + PE_CTRL, PE_CTRL_FINISH_ENABLE);
    const uint64_t t0 = now();
    gp8(GX_CALL_DL);
    gp32(0x00300000u);
    gp32(96);
    gp_bp(GX_BP_DRAWDONE, 0x02);
    gp_pad();
    const uint64_t at = t0 + GC_GX_SLOT + 6 + 576 + 12;
    run_to(at);
    CHECK(!pi_cause(PI_INT_PE_FINISH));
    run_to(at + 1);
    CHECK(pi_cause(PI_INT_PE_FINISH));
    w16(0x1000 + PE_CTRL, PE_CTRL_FINISH_ENABLE | PE_CTRL_FINISH);
    CHECK(!pi_cause(PI_INT_PE_FINISH));
    CHECK_EQ_U64(sys.pe.finishes, 1);
    gp_bp(GX_BP_DRAWDONE, 0x01);                       /* otro valor: no hace nada */
    gp_pad();
    r16(CP_SR);
    CHECK_EQ_U64(sys.pe.finishes, 1);
    teardown();
}

/* ---- estado en texto -------------------------------------------------------------------- */

static char estado[64][64];
static int nestado;

static void collect(void *ctx, const char *text)
{
    (void)ctx;
    if (nestado < 64)
        snprintf(estado[nestado++], sizeof estado[0], "%s", text);
}

static int has_line(const char *text)
{
    for (int k = 0; k < nestado; k++)
        if (strcmp(estado[k], text) == 0)
            return 1;
    return 0;
}

static void test_estado(void)
{
    CHECK(boot() == 0);
    nestado = 0;
    gc_gx_estado(&sys, collect, NULL);
    CHECK(has_line("# gc-ps3 estado gx v1"));
    CHECK(has_line("fifo base 00000000"));
    CHECK(has_line("gp 0"));
    CHECK(has_line("bp fe 00ffffff"));
    CHECK_EQ_U64(nestado, 1 + 12 + 1 + 1);             /* recién encendida */

    fifo_setup(0x1000, 0x800, 0x100, CP_CR_READ | CP_CR_LINK);
    gp_bp(0x41, 0xAA);
    gp8(GX_LOAD_CP);
    gp8(0x50);
    gp32(0x600);
    gp8(GX_LOAD_XF);
    gp32(0x00001008u);
    gp32(0x11);
    gp_pad();
    r16(CP_SR);
    gp8(0x61);
    nestado = 0;
    gc_gx_estado(&sys, collect, NULL);
    CHECK(has_line("fifo base 00100000"));
    CHECK(has_line("fifo end 00100fe0"));
    CHECK(has_line("fifo hi 00000800"));
    CHECK(has_line("fifo lo 00000100"));
    CHECK(has_line("fifo lectura 00100020"));
    CHECK(has_line("fifo banderas 00000141"));         /* lectura, baja (0 < 0x100), enlace */
    CHECK(has_line("pi escritura 00100020"));
    CHECK(has_line("gp 1"));
    CHECK(has_line("bp 41 000000aa"));
    CHECK(has_line("cp 50 00000600"));
    CHECK(has_line("xf 1008 00000011"));
    teardown();
}

const test_case_t gx_tests[] = {
    { "gx: gather pipe a la RAM, vuelta al final y toda la página", test_gather_pipe },
    { "gx: registros del CP, máscaras y mitades", test_cp_registers },
    { "gx: el bloque llega en el acto y la GPU lo lee 1000 ciclos después", test_fifo_sync_event },
    { "gx: presupuesto de la GPU y deuda al turno siguiente", test_fifo_budget },
    { "gx: leer SR o la mitad alta corre la GPU en el acto", test_register_sync },
    { "gx: el FIFO da la vuelta en la CPU, el CP y la GPU", test_fifo_wrap },
    { "gx: marcas alta y baja con su interrupción", test_watermarks },
    { "gx: breakpoint del FIFO", test_breakpoint },
    { "gx: PI_FIFO_RESET", test_pi_fifo_reset },
    { "gx: un evento cercano corta el tramo de la CPU", test_event_cuts_slice },
    { "gx: comandos de CP, BP con máscara, XF, NOPs y desconocidos", test_commands },
    { "gx: cargas de XF y cargas indexadas", test_xf_loads },
    { "gx: tamaño de vértice según VCD y VAT", test_vertex_size },
    { "gx: primitivas saltadas por su tamaño", test_primitive_skip },
    { "gx: display lists", test_display_list },
    { "gx: un comando que cruza bloques del FIFO", test_command_across_blocks },
    { "gx: token del PE con y sin interrupción", test_pe_token },
    { "gx: draw done después de una display list larga", test_pe_finish },
    { "gx: estado en texto para comparar con Dolphin", test_estado },
    { NULL, NULL },
};
