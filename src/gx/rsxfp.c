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
 * El ensamblador de fragment programs del RSX (ver rsxfp.h). Las mismas
 * palabras que cgcomp para las mismas instrucciones (test_rsxfp.c).
 */
#include <string.h>

#include "gx/rsxfp.h"

#define W0_END        (1u << 0)
#define W0_DST(r)     ((uint32_t)(r) << 1)
#define W0_CC_WRITE   (1u << 8)
#define W0_MASK(m)    ((uint32_t)(m) << 9)
#define W0_INPUT(i)   ((uint32_t)(i) << 13)
#define W0_UNIT(u)    ((uint32_t)(u) << 17)
#define W0_OPCODE(o)  ((uint32_t)(o) << 24)
#define W0_NO_DST     (1u << 30 | 63u << 1)
#define W0_SAT        (1u << 31)

#define SRC_TYPE(t)   ((uint32_t)(t))
#define SRC_INDEX(i)  ((uint32_t)(i) << 2)
#define SRC_SWZ(s)    ((uint32_t)(s) << 9)
#define SRC_NEG       (1u << 17)
#define W1_COND(c)    ((uint32_t)(c) << 18)
#define W1_COND_SWZ(s) ((uint32_t)(s) << 21)
#define W1_ABS        (1u << 29)
#define W23_ABS       (1u << 18)
#define W3_TEXCOORD   (0x7FCu << 19)
#define W3_MUL_TC     (1u << 31)

/* Lo que cgcomp pone en una fuente que no se usa: R0.xyzw */
static const rsxfp_src_t unused = { 0, RSXFP_TEMP, 0, RSXFP_XYZW, 0, 0 };

void rsxfp_init(rsxfp_t *p, uint32_t *words, uint32_t max)
{
    memset(p, 0, sizeof *p);
    p->w = words;
    p->max = max;
    p->last = -1;
    p->max_reg = -1;
}

static uint32_t src_word(const rsxfp_src_t *s)
{
    uint32_t w = SRC_TYPE(s->type) | SRC_SWZ(s->swz) | (s->neg ? SRC_NEG : 0);
    if (s->type == RSXFP_TEMP)
        w |= SRC_INDEX(s->index);
    return w;
}

static int fail(rsxfp_t *p, int err)
{
    if (!p->err)
        p->err = err;
    return err;
}

int32_t rsxfp_emit(rsxfp_t *p, const rsxfp_insn_t *in)
{
    if (p->err)
        return p->err;

    /* Fuentes: las que no se dieron (todo en cero) son R0.xyzw */
    rsxfp_src_t s[3];
    int input = -1, konst = 0;
    for (int k = 0; k < 3; k++) {
        s[k] = in->src[k].on ? in->src[k] : unused;
        switch (s[k].type) {
        case RSXFP_TEMP:
            if (s[k].index >= RSXFP_REGS)
                return fail(p, RSXFP_EINSN);
            if (in->src[k].on && s[k].index > p->max_reg)
                p->max_reg = s[k].index;
            break;
        case RSXFP_INPUT:
            if (s[k].index >= RSXFP_INPUTS || (input >= 0 && input != s[k].index))
                return fail(p, RSXFP_EINSN);  /* un solo registro de entrada */
            input = s[k].index;
            break;
        case RSXFP_CONST:
            konst = 1;
            break;
        default:
            return fail(p, RSXFP_EINSN);
        }
    }
    if (in->op > 0x3F || in->mask > 0xF || in->cond > RSXFP_TR || in->unit > 15 ||
        (in->dst != RSXFP_NO_DST && (in->dst < 0 || in->dst >= RSXFP_REGS)))
        return fail(p, RSXFP_EINSN);
    if (p->n + 4u + (konst ? 4u : 0u) > p->max)
        return fail(p, RSXFP_ESPACE);

    uint32_t w0 = W0_OPCODE(in->op) | W0_MASK(in->mask) | W0_UNIT(in->unit) |
                  (in->sat ? W0_SAT : 0) | (in->cc_write ? W0_CC_WRITE : 0);
    if (input >= 0)
        w0 |= W0_INPUT(input);
    if (in->dst == RSXFP_NO_DST) {
        w0 |= W0_NO_DST;
    } else {
        w0 |= W0_DST(in->dst);
        if (in->dst > p->max_reg)
            p->max_reg = in->dst;
    }
    const int texcoord = input >= RSXFP_TC0 && input < RSXFP_TC0 + 10;
    if (texcoord)
        p->texcoords |= (uint16_t)(1u << (input - RSXFP_TC0));
    if (in->op == RSXFP_KIL)
        p->kil = 1;

    p->last = (int32_t)p->n;
    p->w[p->n++] = w0;
    p->w[p->n++] = src_word(&s[0]) | W1_COND(in->cond) | W1_COND_SWZ(in->cond_swz) |
                   (s[0].abs ? W1_ABS : 0);
    p->w[p->n++] = src_word(&s[1]) | (s[1].abs ? W23_ABS : 0);
    p->w[p->n++] = src_word(&s[2]) | (s[2].abs ? W23_ABS : 0) | (texcoord ? W3_TEXCOORD : 0) |
                   (texcoord && in->op == RSXFP_MUL ? W3_MUL_TC : 0);
    p->ninsn++;
    if (!konst)
        return -1;
    const int32_t at = (int32_t)p->n;
    for (int k = 0; k < 4; k++) {
        uint32_t u;
        memcpy(&u, &in->k[k], sizeof u);
        p->w[p->n++] = u;
    }
    return at;
}

int rsxfp_finish(rsxfp_t *p, rsxfp_info_t *info)
{
    if (!p->err && p->last < 0)
        p->err = RSXFP_EINSN;                   /* un programa vacío */
    if (p->err)
        return p->err;
    p->w[p->last] |= W0_END;
    if (info) {
        memset(info, 0, sizeof *info);
        info->ninsn = p->ninsn;
        info->nwords = p->n;
        int regs = p->max_reg + 1;
        if (p->depth && regs < 2)
            regs = 2;
        info->num_regs = regs < 1 ? 1u : (uint32_t)regs;
        info->fp_control = 0x40u | (p->depth ? 0x0Eu : 0u) | (p->kil ? 0x80u : 0u);
        info->texcoords = p->texcoords;
    }
    return 0;
}
