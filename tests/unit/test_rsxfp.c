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
 * Fase 2, etapa D2: fragment programs del RSX (src/gx/rsxfp.c y rsxfp_sim.c).
 * Los programas de tests/unit/data/fp los ensambló cgcomp de PSL1GHT (`make
 * fp-ref`): rsxfp.c tiene que dar las mismas palabras para las mismas
 * instrucciones, y el simulador, los valores que se calculan acá a mano.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx/rsxfp.h"
#include "test.h"

/* ---- el .fpo de cgcomp ---------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Las palabras del microcódigo, en orden lógico; 0 si no se pudo leer */
static uint32_t load_fpo(const char *name, uint32_t *w, uint32_t max)
{
    char path[128];
    snprintf(path, sizeof path, "tests/unit/data/fp/%s.fpo", name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint8_t buf[4096];
    const size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (len < 0x24 || buf[0] != 'F' || buf[1] != 'P')
        return 0;
    const uint32_t off = be32(buf + 0x14);
    uint32_t n = 0;
    for (uint32_t i = off; i + 4 <= len && n < max; i += 4)
        w[n++] = rsxfp_word_swap(be32(buf + i));
    return n;
}

/* ---- armar ------------------------------------------------------------------------------ */

static const rsxfp_src_t none;

static rsxfp_insn_t op3(uint8_t op, int8_t dst, uint8_t mask, rsxfp_src_t a, rsxfp_src_t b, rsxfp_src_t c)
{
    rsxfp_insn_t i;
    memset(&i, 0, sizeof i);
    i.op = op;
    i.dst = dst;
    i.mask = mask;
    i.cond = RSXFP_TR;
    i.cond_swz = RSXFP_XYZW;
    i.src[0] = a;
    i.src[1] = b;
    i.src[2] = c;
    return i;
}

static rsxfp_src_t R(uint8_t r) { return rsxfp_r(r, RSXFP_XYZW); }
static rsxfp_src_t Rs(uint8_t r, uint8_t swz) { return rsxfp_r(r, swz); }
static rsxfp_src_t IN(uint8_t reg) { return rsxfp_in(reg, RSXFP_XYZW); }

#define X 0
#define Y 1
#define Z 2
#define W 3

static void emit_k(rsxfp_t *p, rsxfp_insn_t i, const float k[4])
{
    memcpy(i.k, k, sizeof i.k);
    rsxfp_emit(p, &i);
}

static const float arit_k[4] = { 0.00390625f, 0.5f, 255.0f, -3.0f };

static void build_arit(rsxfp_t *p)
{
    rsxfp_insn_t i;
    const float *k = arit_k;
    i = op3(RSXFP_MOV, 1, 0xF, rsxfp_abs(IN(RSXFP_TC0 + 1)), none, none);
    rsxfp_emit(p, &i);
    emit_k(p, op3(RSXFP_MUL, 2, 0xF, IN(RSXFP_TC0), rsxfp_k(RSXFP_SPLAT(Z)), none), k);
    i = op3(RSXFP_ADD, 3, 0xF, rsxfp_abs(R(1)), rsxfp_neg(rsxfp_abs(Rs(2, RSXFP_SWZ(W, Z, Y, X)))), none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MAD, 4, 0xF, R(1), rsxfp_abs(R(2)), rsxfp_neg(R(3)));
    rsxfp_emit(p, &i);
    i = op3(RSXFP_FLR, 2, 0xF, R(2), none, none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_FRC, 3, 0x3, R(4), none, none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_DP3, 4, 0x1, R(1), R(2), none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_DP4, 4, 0x2, R(1), R(2), none);
    rsxfp_emit(p, &i);
    emit_k(p, op3(RSXFP_MIN, 4, 0x4, Rs(1, RSXFP_SPLAT(X)), rsxfp_k(RSXFP_SPLAT(W)), none), k);
    emit_k(p, op3(RSXFP_MAX, 4, 0x8, Rs(1, RSXFP_SPLAT(Y)), rsxfp_neg(rsxfp_k(RSXFP_SPLAT(Y))), none), k);
    static const uint8_t cmp[6] = { RSXFP_SLT, RSXFP_SGE, RSXFP_SLE, RSXFP_SGT, RSXFP_SNE, RSXFP_SEQ };
    static const uint8_t cmp_mask[6] = { 0xF, 0x3, 0xC, 0xF, 0x5, 0xA };
    for (int c = 0; c < 6; c++) {
        i = c < 3 ? op3(cmp[c], 1, cmp_mask[c], R(2), R(3), none) : op3(cmp[c], 2, cmp_mask[c], R(3), R(4), none);
        rsxfp_emit(p, &i);
    }
    i = op3(RSXFP_RCP, 3, 0x4, Rs(4, RSXFP_SPLAT(W)), none, none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_EX2, 3, 0x8, Rs(2, RSXFP_SPLAT(Y)), none, none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_LG2, 3, 0x1, Rs(4, RSXFP_SPLAT(Z)), none, none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MOV, 0, 0xF, IN(RSXFP_COL0), none, none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_ADD, 0, 0xF, IN(RSXFP_COL1), R(0), none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MAD, 0, 0xF, IN(RSXFP_FOGC), R(1), R(0));
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MUL, 0, 0xF, IN(RSXFP_WPOS), R(0), none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MAD, 0, 0x9, R(3), rsxfp_k(RSXFP_SPLAT(X)), R(4));
    i.sat = 1;
    emit_k(p, i, k);
}

static const float cond_k[4] = { 0.5f, 1.0f, 2.0f, 3.0f };

static void build_cond(rsxfp_t *p)
{
    const float *k = cond_k;
    rsxfp_insn_t i = op3(RSXFP_MOV, 1, 0xF, IN(RSXFP_TC0 + 3), none, none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_SLT, RSXFP_NO_DST, 0x1, Rs(1, RSXFP_SPLAT(X)), rsxfp_k(RSXFP_SPLAT(Y)), none);
    i.cc_write = 1;
    emit_k(p, i, k);
    i = op3(RSXFP_SGE, RSXFP_NO_DST, 0x6, R(1), rsxfp_k(RSXFP_SPLAT(X)), none);
    i.cc_write = 1;
    emit_k(p, i, k);
    emit_k(p, op3(RSXFP_MOV, 2, 0xF, rsxfp_k(RSXFP_XYZW), none, none), k);
    i = op3(RSXFP_MOV, 2, 0xF, R(1), none, none);
    i.cond = RSXFP_GT;
    i.cond_swz = RSXFP_SPLAT(X);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MOV, 2, 0xF, rsxfp_neg(R(1)), none, none);
    i.cond = RSXFP_LT;
    i.cond_swz = RSXFP_SWZ(Y, Z, W, X);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MOV, 0, 0xF, R(2), none, none);
    i.cc_write = 1;
    rsxfp_emit(p, &i);
    i = op3(RSXFP_KIL, RSXFP_NO_DST, 0xF, none, none, none);
    i.cond = RSXFP_LE;
    i.cond_swz = RSXFP_SPLAT(Y);
    rsxfp_emit(p, &i);
    i.cond = RSXFP_NE;
    i.cond_swz = RSXFP_SPLAT(W);
    rsxfp_emit(p, &i);
}

static void build_tex(rsxfp_t *p)
{
    rsxfp_insn_t i = op3(RSXFP_TEX, 2, 0xF, IN(RSXFP_TC0 + 2), none, none);
    i.unit = 3;
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MUL, 1, 0xF, IN(RSXFP_TC0 + 5), R(2), none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_TEX, 1, 0xF, R(1), none, none);
    i.unit = 15;
    rsxfp_emit(p, &i);
    i = op3(RSXFP_TXP, 0, 0xF, IN(RSXFP_TC0 + 7), none, none);
    i.unit = 0;
    rsxfp_emit(p, &i);
    i = op3(RSXFP_ADD, 0, 0xF, R(0), R(1), none);
    rsxfp_emit(p, &i);
    i = op3(RSXFP_MOV, 1, 0x4, Rs(2, RSXFP_SPLAT(W)), none, none);
    rsxfp_emit(p, &i);
    p->depth = 1;
}

/* Arma el programa y lo compara palabra por palabra con el de cgcomp */
static int same_as_cgcomp(const char *name, void (*build)(rsxfp_t *), rsxfp_info_t *info, uint32_t *words)
{
    uint32_t ref[1024];
    const uint32_t n = load_fpo(name, ref, 1024);
    rsxfp_t p;
    rsxfp_init(&p, words, 1024);
    build(&p);
    if (n == 0 || rsxfp_finish(&p, info) != 0)
        return 0;
    if (info->nwords != n) {
        fprintf(stderr, "  %s: %u palabras, cgcomp %u\n", name, (unsigned)info->nwords, (unsigned)n);
        return 0;
    }
    for (uint32_t k = 0; k < n; k++)
        if (words[k] != ref[k]) {
            fprintf(stderr, "  %s: palabra %u = %08x, cgcomp %08x\n", name, (unsigned)k, (unsigned)words[k],
                    (unsigned)ref[k]);
            return 0;
        }
    return 1;
}

static void test_rsxfp_arit(void)
{
    uint32_t w[1024];
    rsxfp_info_t info;
    CHECK(same_as_cgcomp("arit", build_arit, &info, w));
    CHECK_EQ_U64(info.ninsn, 24);
    CHECK_EQ_U64(info.num_regs, 5);
    CHECK_EQ_U64(info.texcoords, 3);
    CHECK_EQ_U64(info.fp_control, 0x40);

    /* El simulador con valores elegidos, contra la misma cuenta en C */
    rsxfp_env_t env;
    memset(&env, 0, sizeof env);
    const float tc0[4] = { 1.25f, -2.5f, 0.75f, 3.0f }, tc1[4] = { -1.5f, 2.0f, -0.25f, 4.0f };
    memcpy(env.in[RSXFP_TC0], tc0, sizeof tc0);
    memcpy(env.in[RSXFP_TC0 + 1], tc1, sizeof tc1);
    const float col0[4] = { 0.25f, 0.5f, 0.75f, 1.0f }, col1[4] = { 0.125f, 0.0f, 0.5f, 0.25f };
    const float wpos[4] = { 10.5f, 20.5f, 0.5f, 1.0f };
    memcpy(env.in[RSXFP_COL0], col0, sizeof col0);
    memcpy(env.in[RSXFP_COL1], col1, sizeof col1);
    memcpy(env.in[RSXFP_WPOS], wpos, sizeof wpos);
    env.in[RSXFP_FOGC][0] = 2.0f;
    env.in[RSXFP_FOGC][1] = 3.0f;
    env.in[RSXFP_FOGC][2] = 4.0f;
    env.in[RSXFP_FOGC][3] = 5.0f;
    rsxfp_out_t out;
    CHECK(rsxfp_run(w, info.nwords, &env, &out) == 0);
    CHECK_EQ_U64(out.steps, 24);

    float r1[4], r2[4], r3[4], r4[4], r0[4];
    for (int c = 0; c < 4; c++) {
        r1[c] = fabsf(tc1[c]);
        r2[c] = tc0[c] * 255.0f;
    }
    for (int c = 0; c < 4; c++)
        r3[c] = fabsf(r1[c]) - fabsf(r2[3 - c]);
    for (int c = 0; c < 4; c++) {
        const float m = r1[c] * fabsf(r2[c]);
        r4[c] = m - r3[c];
    }
    for (int c = 0; c < 4; c++)
        r2[c] = floorf(r2[c]);
    for (int c = 0; c < 2; c++)
        r3[c] = r4[c] - floorf(r4[c]);
    r4[0] = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2];
    r4[1] = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2] + r1[3] * r2[3];
    r4[2] = r1[0] < -3.0f ? r1[0] : -3.0f;
    r4[3] = r1[1] > -0.5f ? r1[1] : -0.5f;
    for (int c = 0; c < 4; c++)
        r1[c] = r2[c] < r3[c] ? 1.0f : 0.0f;
    for (int c = 0; c < 2; c++)
        r1[c] = r2[c] >= r3[c] ? 1.0f : 0.0f;
    for (int c = 2; c < 4; c++)
        r1[c] = r2[c] <= r3[c] ? 1.0f : 0.0f;
    for (int c = 0; c < 4; c++)
        r2[c] = r3[c] > r4[c] ? 1.0f : 0.0f;
    r2[0] = r3[0] != r4[0] ? 1.0f : 0.0f;
    r2[2] = r3[2] != r4[2] ? 1.0f : 0.0f;
    r2[1] = r3[1] == r4[1] ? 1.0f : 0.0f;
    r2[3] = r3[3] == r4[3] ? 1.0f : 0.0f;
    r3[2] = 1.0f / r4[3];
    r3[3] = exp2f(r2[1]);
    r3[0] = log2f(r4[2]);
    for (int c = 0; c < 4; c++) {
        r0[c] = col1[c] + col0[c];
        const float m = env.in[RSXFP_FOGC][c] * r1[c];
        r0[c] = m + r0[c];
        r0[c] = wpos[c] * r0[c];
    }
    for (int c = 0; c < 4; c += 3) {
        const float m = r3[c] * 0.00390625f;
        float v = m + r4[c];
        r0[c] = v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f;
    }
    for (int c = 0; c < 4; c++)
        CHECK(out.color[c] == r0[c]);
}

/* La condición: cc = (x < 1, y >= 0.5, z >= 0.5, -), R2 = c, después R1 donde
 * cc.x > 0 y -R1 donde cc.yzwx < 0, R0 = R2 escribiendo cc; KIL si cc.y <= 0
 * o cc.w != 0 */
static void cond_case(const uint32_t *w, uint32_t n, const float tc[4], int killed, const float color[4])
{
    rsxfp_env_t env;
    memset(&env, 0, sizeof env);
    memcpy(env.in[RSXFP_TC0 + 3], tc, 4 * sizeof(float));
    rsxfp_out_t out;
    CHECK(rsxfp_run(w, n, &env, &out) == 0);
    CHECK_EQ_U64(out.killed, (uint64_t)killed);
    for (int c = 0; c < 4; c++)
        CHECK(out.color[c] == color[c]);
}

static void test_rsxfp_cond(void)
{
    uint32_t w[1024];
    rsxfp_info_t info;
    CHECK(same_as_cgcomp("cond", build_cond, &info, w));
    CHECK_EQ_U64(info.fp_control, 0xC0);
    /* x = 0.5 < 1: cc.x = 1 → R2 = R1; cc.y (y = 2 >= 0.5) = 1, cc.z (z = -1) = 0,
     * cc.w = 0: nada < 0; R0 = (0.5, 2, -1, 7): cc.y = 2 > 0, cc.w = 7 → KIL */
    const float a[4] = { 0.5f, 2.0f, -1.0f, 7.0f };
    cond_case(w, info.nwords, a, 1, a);
    /* x = 3: cc.x = 0 → R2 = c = (0.5, 1, 2, 3); R0.w = 3 → KIL */
    const float b[4] = { 3.0f, 2.0f, 4.0f, 0.0f }, rb[4] = { 0.5f, 1.0f, 2.0f, 3.0f };
    cond_case(w, info.nwords, b, 1, rb);
    /* x = 0.25, w = 0: R0 = R1 con w = 0 y y = 1: no se mata */
    const float c[4] = { 0.25f, 1.0f, 0.0f, 0.0f };
    cond_case(w, info.nwords, c, 0, c);
}

typedef struct tex_log {
    uint32_t n, ord[4], unit[4];
    float    coord[4][4];
} tex_log_t;

static void tex_cb(void *ctx, uint32_t ord, uint32_t unit, const float coord[4], float out[4])
{
    tex_log_t *l = ctx;
    if (l->n < 4) {
        l->ord[l->n] = ord;
        l->unit[l->n] = unit;
        memcpy(l->coord[l->n], coord, 4 * sizeof(float));
        l->n++;
    }
    for (int c = 0; c < 4; c++)
        out[c] = (float)(unit + 1) * 0.125f * (float)(c + 1);
}

static void test_rsxfp_tex(void)
{
    uint32_t w[1024];
    rsxfp_info_t info;
    CHECK(same_as_cgcomp("tex", build_tex, &info, w));
    CHECK_EQ_U64(info.fp_control, 0x4E);
    CHECK_EQ_U64(info.texcoords, 0xA4);
    CHECK_EQ_U64(info.num_regs, 3);

    rsxfp_env_t env;
    memset(&env, 0, sizeof env);
    tex_log_t log;
    memset(&log, 0, sizeof log);
    env.tex = tex_cb;
    env.ctx = &log;
    const float t2[4] = { 0.25f, 0.5f, 0, 1 }, t5[4] = { 2, 4, 6, 8 }, t7[4] = { 1, 2, 3, 4 };
    memcpy(env.in[RSXFP_TC0 + 2], t2, sizeof t2);
    memcpy(env.in[RSXFP_TC0 + 5], t5, sizeof t5);
    memcpy(env.in[RSXFP_TC0 + 7], t7, sizeof t7);
    rsxfp_out_t out;
    CHECK(rsxfp_run(w, info.nwords, &env, &out) == 0);
    CHECK_EQ_U64(log.n, 3);
    CHECK(log.ord[0] == 0 && log.ord[1] == 1 && log.ord[2] == 2);
    CHECK(log.unit[0] == 3 && log.unit[1] == 15 && log.unit[2] == 0);
    CHECK(log.coord[0][0] == 0.25f && log.coord[0][1] == 0.5f);
    /* R1 = TC5 × texel(3) = (2, 4, 6, 8) × 0.5 × (1, 2, 3, 4) */
    CHECK(log.coord[1][0] == 1.0f && log.coord[1][1] == 4.0f && log.coord[1][3] == 16.0f);
    /* TXP: xyz / w */
    CHECK(log.coord[2][0] == 0.25f && log.coord[2][1] == 0.5f && log.coord[2][2] == 0.75f);
    for (int c = 0; c < 4; c++)
        CHECK(out.color[c] == 0.125f * (float)(c + 1) + 2.0f * (float)(c + 1));
    CHECK(out.depth == 2.0f);                   /* texel(3).w = 0.5 × 4 */
}

/* Lo que el ensamblador no acepta */
static void test_rsxfp_errors(void)
{
    uint32_t w[16];
    rsxfp_t p;
    rsxfp_init(&p, w, 16);
    rsxfp_info_t info;
    CHECK(rsxfp_finish(&p, &info) != 0);        /* vacío */

    rsxfp_init(&p, w, 16);
    rsxfp_insn_t i = op3(RSXFP_ADD, 0, 0xF, IN(RSXFP_TC0), IN(RSXFP_COL0), none);
    CHECK(rsxfp_emit(&p, &i) == RSXFP_EINSN);   /* dos entradas */

    rsxfp_init(&p, w, 16);
    i = op3(RSXFP_MOV, 48, 0xF, R(0), none, none);
    CHECK(rsxfp_emit(&p, &i) == RSXFP_EINSN);

    rsxfp_init(&p, w, 7);
    i = op3(RSXFP_MOV, 0, 0xF, rsxfp_k(RSXFP_XYZW), none, none);
    CHECK(rsxfp_emit(&p, &i) == RSXFP_ESPACE);  /* 4 + 4 en 7 */

    rsxfp_init(&p, w, 16);
    i = op3(RSXFP_MOV, 0, 0xF, rsxfp_k(RSXFP_XYZW), none, none);
    i.k[2] = 5.0f;
    CHECK(rsxfp_emit(&p, &i) == 4);             /* la constante va después */
    CHECK(rsxfp_finish(&p, &info) == 0);
    float f;
    memcpy(&f, &w[6], sizeof f);
    CHECK(f == 5.0f);
    CHECK(w[0] & 1u);
}

const test_case_t rsxfp_tests[] = {
    { "rsxfp: aritmética igual a cgcomp y simulada", test_rsxfp_arit },
    { "rsxfp: condiciones y KIL iguales a cgcomp y simulados", test_rsxfp_cond },
    { "rsxfp: texturas y Z iguales a cgcomp y simuladas", test_rsxfp_tex },
    { "rsxfp: errores del ensamblador", test_rsxfp_errors },
    { NULL, NULL },
};
