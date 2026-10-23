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
#include "cpu/huella.h"

static inline uint64_t pair(uint32_t lo, uint32_t hi)
{
    return (uint64_t)lo | (uint64_t)hi << 32;
}

void gekko_estado(const gekko_t *c, uint64_t w[GEKKO_ESTADO_PALABRAS])
{
    int k = 0;
    for (int i = 0; i < 32; i += 2)
        w[k++] = pair(c->gpr[i], c->gpr[i + 1]);
    for (int i = 0; i < 32; i++) {
        w[k++] = c->fpr[i][0];
        w[k++] = c->fpr[i][1];
    }
    w[k++] = pair(c->cr, c->xer);
    w[k++] = pair(c->lr, c->ctr);
    w[k++] = pair(c->msr, c->fpscr);
    w[k++] = pair(c->spr[SPR_SRR0], c->spr[SPR_SRR1]);
    for (int i = 0; i < 8; i += 2)
        w[k++] = pair(c->spr[SPR_GQR0 + i], c->spr[SPR_GQR0 + i + 1]);
    w[k++] = pair(c->spr[SPR_HID0], c->spr[SPR_HID2]);
}

uint64_t gekko_huella(const gekko_t *c)
{
    /* Lo mismo que gekko_estado + una ronda por palabra, sin el arreglo: se
     * llama en cada instrucción de las pruebas contra Dolphin */
    uint64_t h = 0;
    for (int i = 0; i < 32; i += 2)
        h = gekko_huella_ronda(h, pair(c->gpr[i], c->gpr[i + 1]));
    for (int i = 0; i < 32; i++) {
        h = gekko_huella_ronda(h, c->fpr[i][0]);
        h = gekko_huella_ronda(h, c->fpr[i][1]);
    }
    h = gekko_huella_ronda(h, pair(c->cr, c->xer));
    h = gekko_huella_ronda(h, pair(c->lr, c->ctr));
    h = gekko_huella_ronda(h, pair(c->msr, c->fpscr));
    h = gekko_huella_ronda(h, pair(c->spr[SPR_SRR0], c->spr[SPR_SRR1]));
    for (int i = 0; i < 8; i += 2)
        h = gekko_huella_ronda(h, pair(c->spr[SPR_GQR0 + i], c->spr[SPR_GQR0 + i + 1]));
    return gekko_huella_ronda(h, pair(c->spr[SPR_HID0], c->spr[SPR_HID2]));
}
