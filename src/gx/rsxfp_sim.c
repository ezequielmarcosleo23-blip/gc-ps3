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
 * El simulador de fragment programs (ver rsxfp.h), para las pruebas de la PC.
 * Corre el subconjunto que arma rsxfp.c, en fp32 de IEEE: MAD con dos
 * redondeos, RCP, EX2 y LG2 exactos de la libm (el RSX los aproxima: solo se
 * usan donde hay tolerancia). Las condiciones siguen a NV_fragment_program2:
 * cada componente compara el registro de condición (en el orden de su
 * swizzle) con 0, y KIL mata si se cumple en alguno de los cuatro.
 */
#include <math.h>
#include <string.h>

#include "gx/rsxfp.h"

static uint32_t bits(uint32_t v, unsigned at, unsigned n)
{
    return (v >> at) & ((1u << n) - 1u);
}

static float word_float(uint32_t w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    return f;
}

static int test(float v, uint32_t cond)
{
    switch (cond) {
    case RSXFP_FL: return 0;
    case RSXFP_LT: return v < 0.0f;
    case RSXFP_EQ: return v == 0.0f;
    case RSXFP_LE: return v <= 0.0f;
    case RSXFP_GT: return v > 0.0f;
    case RSXFP_NE: return v != 0.0f;
    case RSXFP_GE: return v >= 0.0f;
    default: return 1;
    }
}

typedef struct state {
    float r[RSXFP_REGS][4];
    float cc[4];
} state_t;

/* Lee una fuente: el valor, el swizzle, el valor absoluto y el signo */
static int fetch(const state_t *st, const rsxfp_env_t *env, uint32_t w, uint32_t input, int abs,
                 const float *k, float out[4])
{
    const float *base;
    switch (bits(w, 0, 2)) {
    case RSXFP_TEMP:
        if (bits(w, 8, 1))
            return -1;                          /* registro H: no se usa */
        base = st->r[bits(w, 2, 6) < RSXFP_REGS ? bits(w, 2, 6) : 0];
        break;
    case RSXFP_INPUT:
        if (input >= RSXFP_INPUTS)
            return -1;
        base = env->in[input];
        break;
    case RSXFP_CONST:
        base = k;
        break;
    default:
        return -1;
    }
    for (int c = 0; c < 4; c++) {
        float v = base[bits(w, 9 + 2 * (unsigned)c, 2)];
        if (abs)
            v = fabsf(v);
        if (bits(w, 17, 1))
            v = -v;
        out[c] = v;
    }
    return 0;
}

int rsxfp_run(const uint32_t *words, uint32_t nwords, const rsxfp_env_t *env, rsxfp_out_t *out)
{
    state_t st;
    memset(&st, 0, sizeof st);
    memset(out, 0, sizeof *out);
    uint32_t ord = 0;

    for (uint32_t pc = 0; pc + 4 <= nwords;) {
        const uint32_t w0 = words[pc], w1 = words[pc + 1], w2 = words[pc + 2], w3 = words[pc + 3];
        pc += 4;
        const int konst = bits(w1, 0, 2) == RSXFP_CONST || bits(w2, 0, 2) == RSXFP_CONST ||
                          bits(w3, 0, 2) == RSXFP_CONST;
        float k[4] = { 0, 0, 0, 0 };
        if (konst) {
            if (pc + 4 > nwords)
                return -1;
            for (int c = 0; c < 4; c++)
                k[c] = word_float(words[pc + (uint32_t)c]);
            pc += 4;
        }
        if (bits(w0, 7, 1) || bits(w0, 22, 2))
            return -1;                          /* destino H o media precisión */

        const uint32_t op = bits(w0, 24, 6), input = bits(w0, 13, 4);
        float a[4], b[4], c[4], r[4];
        if (fetch(&st, env, w1, input, (int)bits(w1, 29, 1), k, a) ||
            fetch(&st, env, w2, input, (int)bits(w2, 18, 1), k, b) ||
            fetch(&st, env, w3, input, (int)bits(w3, 18, 1), k, c))
            return -1;

        const uint32_t cond = bits(w1, 18, 3), cswz = bits(w1, 21, 8);
        int pass[4];
        for (int i = 0; i < 4; i++)
            pass[i] = test(st.cc[bits(cswz, 2 * (unsigned)i, 2)], cond);

        switch (op) {
        case RSXFP_NOP:
            continue;
        case RSXFP_MOV: for (int i = 0; i < 4; i++) r[i] = a[i]; break;
        case RSXFP_MUL: for (int i = 0; i < 4; i++) r[i] = a[i] * b[i]; break;
        case RSXFP_ADD: for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; break;
        case RSXFP_MAD:
            for (int i = 0; i < 4; i++) {
                const float m = a[i] * b[i];
                r[i] = m + c[i];
            }
            break;
        case RSXFP_DP3: {
            const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            r[0] = r[1] = r[2] = r[3] = d;
            break;
        }
        case RSXFP_DP4: {
            const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            r[0] = r[1] = r[2] = r[3] = d;
            break;
        }
        case RSXFP_MIN: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; break;
        case RSXFP_MAX: for (int i = 0; i < 4; i++) r[i] = a[i] > b[i] ? a[i] : b[i]; break;
        case RSXFP_SLT: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
        case RSXFP_SGE: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
        case RSXFP_SLE: for (int i = 0; i < 4; i++) r[i] = a[i] <= b[i] ? 1.0f : 0.0f; break;
        case RSXFP_SGT: for (int i = 0; i < 4; i++) r[i] = a[i] > b[i] ? 1.0f : 0.0f; break;
        case RSXFP_SNE: for (int i = 0; i < 4; i++) r[i] = a[i] != b[i] ? 1.0f : 0.0f; break;
        case RSXFP_SEQ: for (int i = 0; i < 4; i++) r[i] = a[i] == b[i] ? 1.0f : 0.0f; break;
        case RSXFP_FRC: for (int i = 0; i < 4; i++) r[i] = a[i] - floorf(a[i]); break;
        case RSXFP_FLR: for (int i = 0; i < 4; i++) r[i] = floorf(a[i]); break;
        case RSXFP_RCP: r[0] = r[1] = r[2] = r[3] = 1.0f / a[0]; break;
        case RSXFP_EX2: r[0] = r[1] = r[2] = r[3] = exp2f(a[0]); break;
        case RSXFP_LG2: r[0] = r[1] = r[2] = r[3] = log2f(a[0]); break;
        case RSXFP_KIL:
            if (pass[0] || pass[1] || pass[2] || pass[3])
                out->killed = 1;
            out->steps++;
            if (bits(w0, 0, 1))
                goto end;
            continue;
        case RSXFP_TEX:
        case RSXFP_TXP: {
            float coord[4] = { a[0], a[1], a[2], a[3] };
            if (op == RSXFP_TXP)
                for (int i = 0; i < 3; i++)
                    coord[i] = a[i] / a[3];
            if (!env->tex)
                return -1;
            env->tex(env->ctx, ord++, bits(w0, 17, 4), coord, r);
            break;
        }
        default:
            return -1;
        }

        if (bits(w0, 31, 1))
            for (int i = 0; i < 4; i++)
                r[i] = r[i] > 0.0f ? (r[i] < 1.0f ? r[i] : 1.0f) : 0.0f;   /* un NaN da 0 */

        const uint32_t mask = bits(w0, 9, 4);
        const int has_dst = !bits(w0, 30, 1);
        const uint32_t dst = bits(w0, 1, 6);
        if (has_dst && dst >= RSXFP_REGS)
            return -1;
        for (int i = 0; i < 4; i++) {
            if (!(mask & (1u << i)) || !pass[i])
                continue;
            if (has_dst)
                st.r[dst][i] = r[i];
            if (bits(w0, 8, 1))
                st.cc[i] = r[i];
        }
        out->steps++;
        if (bits(w0, 0, 1))
            goto end;
    }
    return -1;                                  /* sin la marca de fin */

end:
    memcpy(out->color, st.r[0], sizeof out->color);
    out->depth = st.r[1][2];
    return 0;
}
