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
 * Fragment programs del RSX (Fase 2, etapa D2): un ensamblador del
 * microcódigo (rsxfp.c) y un simulador para la PC (rsxfp_sim.c). En la
 * consola no hay compilador de Cg, así que los programas del TEV se arman acá
 * en el momento; el simulador deja probarlos en el CI.
 *
 * El formato es el de la familia NV40, el mismo que escribe cgcomp de
 * PSL1GHT (y rsxkit, src/fpgen.c). Cada instrucción son 4 palabras y, si
 * alguna fuente es la constante, 4 más con sus floats. Las palabras quedan
 * acá en orden lógico; en la memoria del RSX van con las mitades de 16 bits
 * cambiadas (rsxfp_word_swap). Los campos se cotejaron con programas que
 * cgcomp ensambla (tests/unit/data/fp, `make fp-ref`):
 *
 *   0  bit 0 fin; 1-6 destino (R0-R47); 7 destino H; 8 escribe la
 *      condición; 9-12 máscara xyzw; 13-16 el registro de entrada que lee
 *      (uno por instrucción); 17-20 unidad de textura; 22-23 precisión (0 =
 *      fp32); 24-29 opcode; 30 sin destino; 31 saturar.
 *   1-3  fuentes 0-2: bits 0-1 tipo (temporal, entrada, constante); 2-7
 *      índice del temporal; 8 temporal H; 9-16 swizzle (2 bits por
 *      componente, x abajo); 17 negar. Palabra 1: 18-20 condición, 21-28 su
 *      swizzle, 29 valor absoluto de la fuente 0. Palabras 2 y 3: bit 18 valor
 *      absoluto. Palabra 3: 0x7FC en los bits 19-29 si se lee un TEXCOORD y,
 *      en MUL, además el bit 31 (como cgcomp; con w = 1 en los vértices no
 *      cambia nada).
 *
 * Solo fp32 (registros R): con valores enteros de menos de 2^24 las cuentas
 * son exactas, que es lo que necesita el TEV. No se usan DIV ni RSQ (cgcomp
 * no los ensambla para NV40) ni saltos.
 */
#ifndef GCPS3_GX_RSXFP_H
#define GCPS3_GX_RSXFP_H

#include <stdint.h>

/* Opcodes */
enum {
    RSXFP_NOP = 0x00, RSXFP_MOV = 0x01, RSXFP_MUL = 0x02, RSXFP_ADD = 0x03, RSXFP_MAD = 0x04,
    RSXFP_DP3 = 0x05, RSXFP_DP4 = 0x06, RSXFP_MIN = 0x08, RSXFP_MAX = 0x09, RSXFP_SLT = 0x0A,
    RSXFP_SGE = 0x0B, RSXFP_SLE = 0x0C, RSXFP_SGT = 0x0D, RSXFP_SNE = 0x0E, RSXFP_SEQ = 0x0F,
    RSXFP_FRC = 0x10, RSXFP_FLR = 0x11, RSXFP_KIL = 0x12, RSXFP_TEX = 0x17, RSXFP_TXP = 0x18,
    RSXFP_RCP = 0x1A, RSXFP_EX2 = 0x1C, RSXFP_LG2 = 0x1D,
};

/* Condiciones (sobre el registro de condición) */
enum { RSXFP_FL = 0, RSXFP_LT, RSXFP_EQ, RSXFP_LE, RSXFP_GT, RSXFP_NE, RSXFP_GE, RSXFP_TR };

/* Tipos de fuente */
enum { RSXFP_TEMP = 0, RSXFP_INPUT = 1, RSXFP_CONST = 2 };

/* Registros de entrada */
enum {
    RSXFP_WPOS = 0, RSXFP_COL0 = 1, RSXFP_COL1 = 2, RSXFP_FOGC = 3, RSXFP_TC0 = 4,
    RSXFP_FACING = 14, RSXFP_INPUTS = 15,
};

#define RSXFP_REGS    48                        /* R0-R47 */
#define RSXFP_NO_DST  (-1)                      /* solo la condición (o KIL) */
#define RSXFP_XYZW    0xE4u                     /* swizzle identidad */
#define RSXFP_SWZ(x, y, z, w) ((uint8_t)((x) | (y) << 2 | (z) << 4 | (w) << 6))
#define RSXFP_SPLAT(c)        RSXFP_SWZ(c, c, c, c)

/* Una fuente: temporal R, entrada o la constante de la instrucción. Con on
 * en 0 (la fuente no se dio) queda R0.xyzw, como cgcomp */
typedef struct rsxfp_src {
    uint8_t on;
    uint8_t type;                               /* RSXFP_TEMP, _INPUT, _CONST */
    uint8_t index;                              /* R o registro de entrada */
    uint8_t swz;
    uint8_t neg, abs;
} rsxfp_src_t;

typedef struct rsxfp_insn {
    uint8_t     op;
    int8_t      dst;                            /* R0-R47 o RSXFP_NO_DST */
    uint8_t     mask;                           /* x = 1, y = 2, z = 4, w = 8 */
    uint8_t     sat;
    uint8_t     cc_write;                       /* escribe el registro de condición */
    uint8_t     cond, cond_swz;                 /* escribe (o mata, en KIL) donde se cumple */
    uint8_t     unit;                           /* TEX, TXP */
    rsxfp_src_t src[3];
    float       k[4];                           /* la constante, si alguna fuente la lee */
} rsxfp_insn_t;

/* Lo que hace falta para cargar el programa (rsxFragmentProgram) */
typedef struct rsxfp_info {
    uint32_t ninsn;                             /* instrucciones (sin las constantes) */
    uint32_t nwords;
    uint32_t num_regs;                          /* R0 al último usado */
    uint32_t fp_control;                        /* 0x40 color en fp32, 0x0E Z, 0x80 KIL */
    uint16_t texcoords;                         /* TEXCOORD leídos */
} rsxfp_info_t;

typedef struct rsxfp {
    uint32_t *w;
    uint32_t  n, max;
    int       err;
    int32_t   last;                             /* palabra 0 de la última instrucción */
    uint32_t  ninsn;
    int       max_reg, kil, depth;
    uint16_t  texcoords;
} rsxfp_t;

enum { RSXFP_OK = 0, RSXFP_ESPACE = -1, RSXFP_EINSN = -2 };

void rsxfp_init(rsxfp_t *p, uint32_t *words, uint32_t max);
/* Agrega una instrucción. Devuelve dónde quedó su constante (la palabra del
 * primer float, para cambiarla después), -1 si no lleva, o un error < -1
 * (que queda también en p->err) */
int32_t rsxfp_emit(rsxfp_t *p, const rsxfp_insn_t *i);
/* Marca el fin en la última instrucción. 0, o el primer error */
int rsxfp_finish(rsxfp_t *p, rsxfp_info_t *info);

/* Una palabra en el orden de la memoria del RSX (y de vuelta) */
static inline uint32_t rsxfp_word_swap(uint32_t w)
{
    return w << 16 | w >> 16;
}

/* Fuentes de uso común */
static inline rsxfp_src_t rsxfp_r(uint8_t r, uint8_t swz)
{
    rsxfp_src_t s = { 1, RSXFP_TEMP, r, swz, 0, 0 };
    return s;
}

static inline rsxfp_src_t rsxfp_in(uint8_t reg, uint8_t swz)
{
    rsxfp_src_t s = { 1, RSXFP_INPUT, reg, swz, 0, 0 };
    return s;
}

static inline rsxfp_src_t rsxfp_k(uint8_t swz)
{
    rsxfp_src_t s = { 1, RSXFP_CONST, 0, swz, 0, 0 };
    return s;
}

static inline rsxfp_src_t rsxfp_neg(rsxfp_src_t s)
{
    s.neg ^= 1;
    return s;
}

static inline rsxfp_src_t rsxfp_abs(rsxfp_src_t s)
{
    s.abs = 1;
    return s;
}

/* ---- simulador (rsxfp_sim.c) ---- */

/* Una textura: ord es el número de TEX/TXP ejecutado (0, 1, ...), coord la
 * coordenada (dividida por w en TXP); out en floats como el RSX */
typedef void (*rsxfp_tex_fn)(void *ctx, uint32_t ord, uint32_t unit, const float coord[4], float out[4]);

typedef struct rsxfp_env {
    float        in[RSXFP_INPUTS][4];
    rsxfp_tex_fn tex;
    void        *ctx;
} rsxfp_env_t;

typedef struct rsxfp_out {
    float    color[4];                          /* R0 */
    float    depth;                             /* R1.z */
    int      killed;
    uint32_t steps;                             /* instrucciones ejecutadas */
} rsxfp_out_t;

/* Corre las palabras (orden lógico) hasta el fin. 0, o -1 si encuentra algo
 * que no simula (opcode, destino H, sin fin) */
int rsxfp_run(const uint32_t *words, uint32_t nwords, const rsxfp_env_t *env, rsxfp_out_t *out);

#endif
