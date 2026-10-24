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
 * DOL de prueba "gx_fifo" (Fase 2, etapa A): el FIFO de GX de punta a punta,
 * sin dibujar. Con una mini-librería propia (gxlib, sin libogc):
 *
 *   1. los registros del CP, del PE y del PI al encender;
 *   2. comandos de BP (con la máscara), CP y XF por el gather pipe, y la
 *      espera leyendo SR;
 *   3. un FIFO de 4 KB que da varias vueltas, regulado por la distancia;
 *   4. las marcas alta y baja con su interrupción, como el manejador de libogc;
 *   5. display lists (una dentro de otra se ignora);
 *   6. cargas indexadas de XF (matrices y luces);
 *   7. primitivas que la GPU salta (directas, indexadas, una de 640 bytes) y
 *      opcodes desconocidos;
 *   8. el Pixel Engine: token con y sin interrupción, dos juntos, "draw done"
 *      y un token después de una display list larga;
 *   9. el breakpoint del FIFO;
 *  10. PI_FIFO_RESET (GXAbortFrame) y el FIFO de nuevo.
 *
 * Imprime valores que dependen del tiempo de la GPU (lecturas de SR, ticks del
 * timebase): con el modelo de Dolphin son los mismos en los dos (D-042). La
 * salida esperada es la del runner revisada a mano; que es la de Dolphin lo
 * prueba la traza instrucción por instrucción, y el estado de BP, CP y XF al
 * final se compara aparte (tests/dol/dolphin/gx_fifo.gx.txt).
 *
 * Solo para el DOL: no compila en la PC.
 */
#include "gxlib.h"
#include "rt.h"

#define VECTOR    0x80000500u
#define FIFO_SIZE 4096u
#define MAX_WAIT  2000000u

static uint8_t fifo[FIFO_SIZE] __attribute__((aligned(32)));
static uint8_t dl_mem[1024] __attribute__((aligned(32)));
static uint8_t big_dl[1024] __attribute__((aligned(32)));
static uint32_t arrays[256] __attribute__((aligned(32)));
static uint8_t pos_array[64] __attribute__((aligned(32)));

static int fails;

/* ---- interrupción externa ---------------------------------------------------------- */

volatile uint32_t irq_causes;                    /* causas vistas, que main atiende */
volatile uint32_t irq_count;
volatile uint32_t irq_tb;

/* Copiado a 0x500; corre sin traducción (las variables, restando 0x80000000).
 * Anota las causas pendientes y habilitadas y las enmascara en el PI hasta que
 * main las atienda. Usa r3-r5 (guardados en SPRG0-2) y no toca CR. */
extern const uint32_t ext_handler[], ext_handler_end[];
__asm__("    .section .text\n"
        "    .globl ext_handler, ext_handler_end\n"
        "ext_handler:\n"
        "    mtsprg  0, 3\n"
        "    mtsprg  1, 4\n"
        "    mtsprg  2, 5\n"
        "    lis     3, 0x0C00\n"
        "    lwz     4, 0x3000(3)\n"                 /* INTSR */
        "    lwz     5, 0x3004(3)\n"                 /* INTMR */
        "    and     4, 4, 5\n"
        "    andc    5, 5, 4\n"
        "    stw     5, 0x3004(3)\n"
        "    lis     3, (irq_causes - 0x80000000)@ha\n"
        "    lwz     5, (irq_causes - 0x80000000)@l(3)\n"
        "    or      5, 5, 4\n"
        "    stw     5, (irq_causes - 0x80000000)@l(3)\n"
        "    lis     3, (irq_count - 0x80000000)@ha\n"
        "    lwz     5, (irq_count - 0x80000000)@l(3)\n"
        "    addi    5, 5, 1\n"
        "    stw     5, (irq_count - 0x80000000)@l(3)\n"
        "    mftb    5\n"
        "    lis     3, (irq_tb - 0x80000000)@ha\n"
        "    stw     5, (irq_tb - 0x80000000)@l(3)\n"
        "    mfsprg  5, 2\n"
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

static uint32_t irq_off(void)
{
    uint32_t msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr & ~0x8000u) : "memory");
    return msr;
}

static void irq_restore(uint32_t msr)
{
    __asm__ volatile("mtmsr %0" : : "r"(msr) : "memory");
}

/* Habilita causas en el PI (con EE apagado: el manejador también escribe INTMR) */
static void unmask(uint32_t causes)
{
    const uint32_t msr = irq_off();
    PI_REG(PI_INTMR) |= causes;
    irq_restore(msr);
}

/* Las causas pedidas que ya llegaron, y las borra */
static uint32_t take(uint32_t causes)
{
    const uint32_t msr = irq_off();
    const uint32_t got = irq_causes & causes;
    irq_causes &= ~got;
    irq_restore(msr);
    return got;
}

/* Espera una causa; 0 si no llegó */
static int wait_irq(uint32_t cause)
{
    for (uint32_t n = 0; n < MAX_WAIT; n++)
        if (take(cause))
            return 1;
    return 0;
}

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        OSReport("gx_fifo: FALLO %s\n", what);
    }
}

static uint32_t off(uint32_t phys)
{
    return phys - gx_phys(fifo);
}

static void wait_idle(const char *what)
{
    const uint32_t n = gx_wait_idle(MAX_WAIT);
    check(n != 0, what);
}

/* ---- 1. encendido ----------------------------------------------------------------- */

static void power_on(void)
{
    OSReport("gx_fifo: encendido: CP SR %04x CR %04x, PE ctrl %04x token %04x\n",
             (unsigned)CP_REG(CP_SR), (unsigned)CP_REG(CP_CR), (unsigned)PE_REG(PE_CTRL),
             (unsigned)PE_REG(PE_TOKEN));
    OSReport("gx_fifo: encendido: CP base %08x fin %08x distancia %08x, PI %08x %08x %08x\n",
             (unsigned)cp_read32(CP_BASE), (unsigned)cp_read32(CP_END), (unsigned)cp_read32(CP_DIST),
             (unsigned)PI_REG(PI_FIFO_BASE), (unsigned)PI_REG(PI_FIFO_END),
             (unsigned)PI_REG(PI_FIFO_WPTR));
}

/* ---- 2. comandos ---------------------------------------------------------------------- */

static const uint32_t identity[12] = {
    0x3F800000u, 0, 0, 0, 0, 0x3F800000u, 0, 0, 0, 0, 0x3F800000u, 0,
};

static void commands(void)
{
    gx_init_fifo(fifo, FIFO_SIZE, FIFO_SIZE - 1024, FIFO_SIZE / 2, CP_CR_READ | CP_CR_LINK);
    OSReport("gx_fifo: FIFO de %u bytes, CR %04x, PI escritura +%u\n", (unsigned)FIFO_SIZE,
             (unsigned)CP_REG(CP_CR), (unsigned)off(PI_REG(PI_FIFO_WPTR)));

    /* CP: formatos de vértice, arreglos y matrices */
    gx_cp(0x20, 0);                              /* como libogc al iniciar */
    gx_cp(0x50, 0x00002200u);                    /* VCD: posición y color 0 directos */
    gx_cp(0x60, 0);
    for (uint32_t k = 0; k < 8; k++) {
        gx_cp(0x70 + k, 0x40000000u | 0x14009u | k << 4);
        gx_cp(0x80 + k, 0x80000000u + k);
        gx_cp(0x90 + k, k << 27);
    }
    gx_cp(0x30, 0x0003CF3Cu);                    /* MATINDEX_A */
    gx_cp(0x40, 0x00F3CF3Cu & 0xFFFFFFu);        /* MATINDEX_B */
    gx_cp(0xA5, 0xFFFFFFFFu);                    /* base: 26 bits */
    gx_cp(0xB5, 0x1234u);                        /* paso: 8 bits */
    gx_cp(0xC0, 0x1u);                           /* no existe: se ignora */

    /* XF: matrices, registros y una carga que cruza a los registros */
    gx_xf(0x000, 12, identity);
    gx_xf(0x400, 9, identity);
    gx_xf(0x500, 12, identity);
    gx_xf1(0x1008, 0x00000001u);                 /* INVTXSPEC: un color */
    gx_xf1(0x1009, 0x00000001u);                 /* numChan */
    gx_xf1(0x1018, 0x0003CF3Cu);                 /* MatrixIndexA, como en CP */
    gx_xf1(0x1019, 0x00F3CF3Cu);
    static const uint32_t viewport[6] = {
        0x43A00000u, 0xC3700000u, 0x4B7FFFFFu, 0x43A00000u, 0x43700000u, 0x4B7FFFFFu,
    };
    gx_xf(0x101A, 6, viewport);
    static const uint32_t cross[4] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };
    gx_xf(0x0FFE, 4, cross);                     /* 0xFFE, 0xFFF, 0x1000, 0x1001 */
    gx_xf(0x1056, 4, cross);                     /* se corta en 0x1057 */

    /* BP, con la máscara: solo los bits 0x0000FF de la escritura siguiente */
    gx_bp(0x00, 0x00000410u);                    /* genMode */
    gx_bp(0x20, 0x00000000u);                    /* scissor */
    gx_bp(0x21, 0x001DF27Fu);
    gx_bp(0x41, 0x0000B000u);
    gx_bp(BP_MASK, 0x0000FFu);
    gx_bp(0x41, 0x00FFFF5Au);                    /* queda 0x00B05A */
    gx_bp(0x42, 0x00ABCDEFu);                    /* la máscara ya volvió */
    for (uint32_t k = 0; k < 16; k++)
        gx_bp(0xC0 + k, k * 0x010101u);          /* TEV */
    gx_flush();

    const uint32_t polls = gx_wait_idle(MAX_WAIT);
    check(polls != 0, "comandos: la GPU no vacio el FIFO");
    OSReport("gx_fifo: comandos: %u lecturas de SR, lectura +%u escritura +%u, distancia %u\n",
             (unsigned)polls, (unsigned)off(cp_read32(CP_RPTR)), (unsigned)off(cp_read32(CP_WPTR)),
             (unsigned)cp_read32(CP_DIST));
}

/* ---- 3. vueltas -------------------------------------------------------------------------- */

/* Cada vuelta del bucle: 14 bytes de comandos y un bloque de NOPs (192 ciclos
 * de GPU por 8 stw): la GPU queda atrás y hay que esperarla por la distancia. */
static void nop_block(void)
{
    for (int k = 0; k < 8; k++)
        WGPIPE32 = 0;
}

static void wraps(void)
{
    uint32_t waits = 0, laps = 0, last = cp_read32(CP_WPTR);
    for (uint32_t k = 0; k < 600; k++) {
        gx_xf1(k & 0xFFu, k);                    /* 9 bytes */
        gx_bp(0x30 + (k & 7u), k);               /* 5 bytes */
        nop_block();
        const uint32_t w = CP_REG(CP_WPTR) | (uint32_t)CP_REG(CP_WPTR + 2) << 16;
        if (w < last)
            laps++;
        last = w;
        if ((k & 7u) == 7u && cp_read32(CP_DIST) > FIFO_SIZE / 2) {
            waits++;
            uint32_t n = 0;
            while (cp_read32(CP_DIST) > FIFO_SIZE / 4 && n < MAX_WAIT)
                n++;
        }
    }
    gx_flush();
    wait_idle("vueltas: la GPU no vacio el FIFO");
    OSReport("gx_fifo: vueltas: 27600 bytes, %u vueltas, %u esperas, lectura +%u\n",
             (unsigned)laps, (unsigned)waits, (unsigned)off(cp_read32(CP_RPTR)));
    check(laps >= 6, "vueltas: menos de 6");
}

/* ---- 4. marcas ---------------------------------------------------------------------------- */

static void watermarks(void)
{
    const uint32_t hi = FIFO_SIZE / 2, lo = FIFO_SIZE / 4;
    cp_write32(CP_HI, hi);
    cp_write32(CP_LO, lo);
    CP_REG(CP_CR) = CP_CR_READ | CP_CR_LINK | CP_CR_OVF_INT;
    take(PI_INT_CP);
    unmask(PI_INT_CP);

    uint32_t overflows = 0, underflows = 0, max_dist = 0;
    uint32_t ovf_sr = 0, undf_sr = 0;
    for (uint32_t k = 0; k < 400; k++) {
        gx_bp(0x28 + (k & 3u), k);
        nop_block();
        if (!take(PI_INT_CP))
            continue;
        /* Desborde: como __GXCPInterruptHandler, se pasa a esperar el vaciado */
        overflows++;
        const uint32_t d = CP_REG(CP_DIST) | (uint32_t)CP_REG(CP_DIST + 2) << 16;
        if (d > max_dist)
            max_dist = d;
        CP_REG(CP_CR) = CP_CR_READ | CP_CR_LINK | CP_CR_UNDF_INT;
        ovf_sr = CP_REG(CP_SR);                  /* sincroniza y baja la causa vieja */
        unmask(PI_INT_CP);
        if (!wait_irq(PI_INT_CP)) {
            check(0, "marcas: no llego la interrupcion de vaciado");
            break;
        }
        underflows++;
        undf_sr = CP_REG(CP_SR);
        CP_REG(CP_CR) = CP_CR_READ | CP_CR_LINK | CP_CR_OVF_INT;
        CP_REG(CP_SR);
        unmask(PI_INT_CP);
    }
    CP_REG(CP_CR) = CP_CR_READ | CP_CR_LINK;
    CP_REG(CP_SR);
    take(PI_INT_CP);
    gx_flush();
    wait_idle("marcas: la GPU no vacio el FIFO");
    OSReport("gx_fifo: marcas: %u desbordes y %u vaciados, distancia maxima %u, SR %04x y %04x\n",
             (unsigned)overflows, (unsigned)underflows, (unsigned)max_dist, (unsigned)ovf_sr,
             (unsigned)undf_sr);
    check(overflows > 0 && overflows == underflows, "marcas: desbordes y vaciados");
    check(max_dist <= hi + 160, "marcas: el FIFO paso la marca alta");
    cp_write32(CP_HI, FIFO_SIZE - 1024);
    cp_write32(CP_LO, FIFO_SIZE / 2);
}

/* ---- 5. display lists ------------------------------------------------------------------- */

static uint32_t build_dl(void)
{
    gx_dl_t dl = { dl_mem, 0, sizeof dl_mem };
    dl_bp(&dl, 0x43, 0x000123u);
    dl_cp(&dl, 0x50, 0x00002200u);
    dl_xf1(&dl, 0x1009, 0x00000001u);
    dl_u8(&dl, GX_CALL_DL);                      /* otra dentro: se ignora */
    dl_u32(&dl, gx_phys(dl_mem));
    dl_u32(&dl, 64);
    for (uint32_t k = 0; k < 8; k++)
        dl_bp(&dl, 0x44, 0x000100u + k);         /* el último queda */
    dl_xf1(&dl, 0x0020, 0x5555AAAAu);
    return dl_end(&dl);
}

static void display_lists(void)
{
    const uint32_t size = build_dl();
    gx_call_dl(dl_mem, size);
    gx_bp(0x46, 0x000001u);
    gx_call_dl(dl_mem, size);
    gx_bp(0x46, 0x000002u);
    gx_call_dl((const uint8_t *)dl_mem + 5, size + 7);   /* se alinean dirección y largo */
    gx_flush();
    wait_idle("display lists: la GPU no vacio el FIFO");
    OSReport("gx_fifo: display lists: 3 llamadas de %u bytes\n", (unsigned)size);
}

/* ---- 6. cargas indexadas ------------------------------------------------------------------ */

static void indexed(void)
{
    for (uint32_t k = 0; k < 256; k++)
        arrays[k] = 0xA5000000u + k;
    dc_flush(arrays, sizeof arrays);
    const uint32_t base = gx_phys(arrays);
    gx_cp(0xAC, base);                           /* XF_A: matrices de posición, 48 bytes */
    gx_cp(0xBC, 48);
    gx_cp(0xAD, base + 0x100);                   /* XF_B: normales, 36 bytes */
    gx_cp(0xBD, 36);
    gx_cp(0xAE, base + 0x200);                   /* XF_C: post, 48 bytes */
    gx_cp(0xBE, 48);
    gx_cp(0xAF, base + 0x300);                   /* XF_D: luces, 64 bytes */
    gx_cp(0xBF, 64);
    gx_xf_indexed(GX_LOAD_INDX_A, 2, 0x018, 12);
    gx_xf_indexed(GX_LOAD_INDX_B, 1, 0x403, 9);
    gx_xf_indexed(GX_LOAD_INDX_C, 0, 0x50C, 12);
    gx_xf_indexed(GX_LOAD_INDX_D, 0, 0x610, 16);
    gx_flush();
    wait_idle("cargas indexadas: la GPU no vacio el FIFO");
    OSReport("gx_fifo: cargas indexadas: 4 (matriz, normal, post, luz)\n");
}

/* ---- 7. primitivas ------------------------------------------------------------------------ */

static void vertex_f32(uint32_t x, uint32_t y, uint32_t z, uint32_t rgba)
{
    WGPIPE32 = x;
    WGPIPE32 = y;
    WGPIPE32 = z;
    WGPIPE32 = rgba;
}

static void primitives(void)
{
    /* Formato 0: XYZ float y RGBA8888; XF con un color, sin normales ni texturas */
    gx_cp(0x50, 0x00002200u);
    gx_cp(0x60, 0);
    gx_cp(0x70, 0x00014009u);
    gx_xf1(0x1008, 0x00000001u);
    gx_begin(GX_TRIANGLES, 0, 3);
    vertex_f32(0x00000000u, 0x00000000u, 0x00000000u, 0xFF0000FFu);
    vertex_f32(0x42C80000u, 0x00000000u, 0x00000000u, 0x00FF00FFu);
    vertex_f32(0x00000000u, 0x42C80000u, 0x00000000u, 0x0000FFFFu);
    gx_begin(GX_TRIANGLE_STRIP, 0, 40);          /* 640 bytes: cruza 20 bloques */
    for (uint32_t k = 0; k < 40; k++)
        vertex_f32(k << 20, (k & 1) << 24, 0x3F800000u, 0x80808080u | k);
    gx_bp(0x43, 0x000777u);

    /* Formato 1: posición con índice de 8 bits a un arreglo de s16, sin color */
    for (uint32_t k = 0; k < 64; k++)
        pos_array[k] = (uint8_t)k;
    dc_flush(pos_array, sizeof pos_array);
    gx_cp(0xA0, gx_phys(pos_array));
    gx_cp(0xB0, 6);
    gx_cp(0x50, 0x00000400u);
    gx_cp(0x71, 0x00000007u);                    /* XYZ s16 */
    gx_xf1(0x1008, 0x00000000u);
    gx_begin(GX_TRIANGLE_FAN, 1, 4);
    WGPIPE32 = 0x00010203u;
    gx_begin(GX_POINTS, 1, 0);                   /* sin vértices */

    /* Desconocidos que el hardware ignora, y NOPs sueltos */
    WGPIPE8 = GX_METRICS;
    WGPIPE8 = GX_INVL_VC;
    WGPIPE8 = 0x01;
    WGPIPE8 = GX_NOP;
    WGPIPE8 = GX_NOP;
    gx_bp(0x42, 0x5A5A5Au);                      /* para ver que se siguió bien */
    gx_flush();
    wait_idle("primitivas: la GPU no vacio el FIFO");
    OSReport("gx_fifo: primitivas: 4 (3, 40, 4 y 0 vertices) y 3 opcodes desconocidos\n");
}

/* ---- 8. Pixel Engine ------------------------------------------------------------------------ */

static void pixel_engine(void)
{
    /* Token sin interrupción: se lee el registro hasta que cambia */
    PE_REG(PE_CTRL) = 0;
    uint32_t t0 = mftb(), n = 0;
    gx_bp(BP_TOKEN, 0x1234u);
    gx_flush();
    while (PE_REG(PE_TOKEN) != 0x1234u && n < MAX_WAIT)
        n++;
    uint32_t dt = mftb() - t0;
    check(n < MAX_WAIT, "PE: el token no llego");
    OSReport("gx_fifo: token %04x sin interrupcion: %u lecturas, %u ticks\n",
             (unsigned)PE_REG(PE_TOKEN), (unsigned)n, (unsigned)dt);

    /* Con interrupción */
    PE_REG(PE_CTRL) = PE_TOKEN_ENABLE;
    take(PI_INT_PE_TOKEN);
    unmask(PI_INT_PE_TOKEN);
    t0 = mftb();
    gx_bp(BP_TOKEN_INT, 0xBEEFu);
    gx_flush();
    check(wait_irq(PI_INT_PE_TOKEN), "PE: no llego la interrupcion del token");
    dt = irq_tb - t0;
    OSReport("gx_fifo: token %04x con interrupcion: a los %u ticks, INTSR %08x\n",
             (unsigned)PE_REG(PE_TOKEN), (unsigned)dt, (unsigned)(PI_REG(PI_INTSR) & 0xE00u));
    PE_REG(PE_CTRL) = PE_TOKEN_ENABLE | PE_TOKEN_ACK;
    OSReport("gx_fifo: token atendido: INTSR %08x\n", (unsigned)(PI_REG(PI_INTSR) & 0xE00u));

    /* Dos juntos: una sola interrupción, con el segundo */
    const uint32_t before = irq_count;
    unmask(PI_INT_PE_TOKEN);
    gx_bp(BP_TOKEN_INT, 0x1111u);
    gx_bp(BP_TOKEN_INT, 0x2222u);
    gx_flush();
    check(wait_irq(PI_INT_PE_TOKEN), "PE: no llego la interrupcion de los dos tokens");
    PE_REG(PE_CTRL) = PE_TOKEN_ENABLE | PE_TOKEN_ACK;
    OSReport("gx_fifo: dos tokens: %04x, %u interrupcion\n", (unsigned)PE_REG(PE_TOKEN),
             (unsigned)(irq_count - before));

    /* Draw done */
    PE_REG(PE_CTRL) = PE_TOKEN_ENABLE | PE_FINISH_ENABLE;
    take(PI_INT_PE_FINISH);
    unmask(PI_INT_PE_FINISH);
    t0 = mftb();
    gx_bp(BP_DRAWDONE, 0x02u);
    gx_flush();
    check(wait_irq(PI_INT_PE_FINISH), "PE: no llego draw done");
    dt = irq_tb - t0;
    PE_REG(PE_CTRL) = PE_TOKEN_ENABLE | PE_FINISH_ENABLE | PE_FINISH_ACK;
    OSReport("gx_fifo: draw done a los %u ticks, INTSR %08x\n", (unsigned)dt,
             (unsigned)(PI_REG(PI_INTSR) & 0xE00u));

    /* Token después de una display list de 1024 NOPs (6144 ciclos de GPU) */
    dc_flush(big_dl, sizeof big_dl);
    unmask(PI_INT_PE_TOKEN);
    t0 = mftb();
    gx_call_dl(big_dl, sizeof big_dl);
    gx_bp(BP_TOKEN_INT, 0x3333u);
    gx_flush();
    check(wait_irq(PI_INT_PE_TOKEN), "PE: no llego el token despues de la lista");
    dt = irq_tb - t0;
    PE_REG(PE_CTRL) = PE_TOKEN_ENABLE | PE_FINISH_ENABLE | PE_TOKEN_ACK;
    OSReport("gx_fifo: token %04x despues de una lista larga: a los %u ticks\n",
             (unsigned)PE_REG(PE_TOKEN), (unsigned)dt);
    PE_REG(PE_CTRL) = 0;
    wait_idle("PE: la GPU no vacio el FIFO");
}

/* ---- 9. breakpoint ------------------------------------------------------------------------ */

static void breakpoint(void)
{
    const uint32_t w = cp_read32(CP_WPTR);
    const uint32_t bp = w + 3 * 32 >= gx_phys(fifo) + FIFO_SIZE ? w + 3 * 32 - FIFO_SIZE : w + 3 * 32;
    cp_write32(CP_BP, bp);
    CP_REG(CP_CR) = CP_CR_READ | CP_CR_LINK | CP_CR_BP_ENABLE | CP_CR_BP_INT;
    take(PI_INT_CP);
    unmask(PI_INT_CP);
    for (uint32_t k = 0; k < 40; k++)            /* 200 bytes: más allá del breakpoint */
        gx_bp(0xE0 + (k & 7u), k);
    gx_flush();
    check(wait_irq(PI_INT_CP), "breakpoint: no llego la interrupcion");
    const uint16_t sr = CP_REG(CP_SR);
    const uint32_t r = cp_read32(CP_RPTR);
    OSReport("gx_fifo: breakpoint en +%u: SR %04x, lectura +%u, distancia %u\n", (unsigned)off(bp),
             (unsigned)sr, (unsigned)off(r), (unsigned)cp_read32(CP_DIST));
    check(r == bp && (sr & CP_SR_BREAKPOINT), "breakpoint: no paro donde debia");
    CP_REG(CP_CR) = CP_CR_READ | CP_CR_LINK;     /* sigue */
    const uint32_t polls = gx_wait_idle(MAX_WAIT);
    check(polls != 0, "breakpoint: la GPU no siguio");
    OSReport("gx_fifo: breakpoint apagado: %u lecturas de SR, SR %04x, INTSR %08x\n",
             (unsigned)polls, (unsigned)CP_REG(CP_SR), (unsigned)(PI_REG(PI_INTSR) & 0xE00u));
    take(PI_INT_CP);
}

/* ---- 10. PI_FIFO_RESET ------------------------------------------------------------------ */

static void abort_frame(void)
{
    gx_bp(0x49, 0x111111u);
    gx_bp(0x4A, 0x222222u);                      /* 10 bytes más en el gather pipe */
    PI_REG(PI_FIFO_RESET) = 1;
    for (volatile int k = 0; k < 100; k++)
        ;
    PI_REG(PI_FIFO_RESET) = 0;
    OSReport("gx_fifo: PI_FIFO_RESET: CR %04x, marca alta %08x, baja %08x\n",
             (unsigned)CP_REG(CP_CR), (unsigned)cp_read32(CP_HI), (unsigned)cp_read32(CP_LO));
    gx_init_fifo(fifo, FIFO_SIZE, FIFO_SIZE - 1024, FIFO_SIZE / 2, CP_CR_READ | CP_CR_LINK);
    gx_bp(0x4D, 0x000333u);
    gx_flush();
    wait_idle("PI_FIFO_RESET: la GPU no vacio el FIFO");
    OSReport("gx_fifo: FIFO de nuevo: lectura +%u, SR %04x\n", (unsigned)off(cp_read32(CP_RPTR)),
             (unsigned)CP_REG(CP_SR));
}

int main(void)
{
    OSReport("gx_fifo: FIFO de GX, Command Processor y Pixel Engine\n");
    install();
    PI_REG(PI_INTMR) = 0;
    uint32_t msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr | 0x8000u));     /* EE */

    power_on();
    commands();
    wraps();
    watermarks();
    display_lists();
    indexed();
    primitives();
    pixel_engine();
    breakpoint();
    abort_frame();

    OSReport("gx_fifo: %u interrupciones; %s\n", (unsigned)irq_count, fails ? "FALLO" : "OK");
    exit(fails ? 1 : 0);
}
