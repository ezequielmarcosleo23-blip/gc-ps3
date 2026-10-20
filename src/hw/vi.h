/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS)
 * Partes portadas de Dolphin, Copyright 2008 Dolphin Emulator Project
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
 * Video Interface (0x0C002000): genera la señal de video a partir del XFB
 * (el búfer de imagen en YUV 4:2:2 que deja el programa en la RAM), lleva la
 * cuenta de medias líneas y dispara las interrupciones de pantalla (DI0-DI3)
 * que los programas usan para sincronizarse con el retrazo.
 *
 * Como Source/Core/Core/HW/VideoInterface.cpp de Dolphin (commit cf5cb7ef):
 * registros de 16 bits (los de 32 son dos mitades, la alta primero), un
 * evento del planificador por media línea y los mismos valores al encender.
 * La duración de la media línea sale de HLW y del reloj de VI (27 o 54 MHz):
 * en NTSC 480i son 15 444 ciclos de CPU y 525 medias líneas por campo
 * (59,94 campos por segundo).
 *
 * Cada campo que termina se informa con gc_vi_field_t (dirección del XFB,
 * ancho, separación entre líneas y alto): lo usa la etapa E para mostrarlo.
 * La VI también marca cuándo el SI sondea los mandos: 15 medias líneas
 * después de cada comienzo de campo y luego cada X líneas (SIPOLL).
 */
#ifndef GCPS3_HW_VI_H
#define GCPS3_HW_VI_H

#include <stdint.h>

/* Registros (desplazamiento desde 0x0C002000; los de 32 bits empiezan en la mitad alta) */
enum {
    VI_VTR = 0x00, VI_DCR = 0x02, VI_HTR0 = 0x04, VI_HTR1 = 0x08, VI_VTO = 0x0C, VI_VTE = 0x10,
    VI_BBOI = 0x14, VI_BBEI = 0x18, VI_TFBL = 0x1C, VI_TFBR = 0x20, VI_BFBL = 0x24, VI_BFBR = 0x28,
    VI_DPV = 0x2C, VI_DPH = 0x2E, VI_DI0 = 0x30, VI_DI1 = 0x34, VI_DI2 = 0x38, VI_DI3 = 0x3C,
    VI_DL0 = 0x40, VI_DL1 = 0x44, VI_HSW = 0x48, VI_HSR = 0x4A, VI_FCT0 = 0x4C, VI_AA = 0x68,
    VI_CLOCK = 0x6C, VI_DTV = 0x6E, VI_FBWIDTH = 0x70, VI_BORDER_END = 0x72, VI_BORDER_START = 0x74,
    VI_REGS_SIZE = 0x80,
};

/* Bits de DI0-DI3 (registro de 32 bits) */
#define VI_DI_INT   0x80000000u      /* la interrupción está activa (se borra escribiendo 0) */
#define VI_DI_MASK  0x10000000u      /* habilitada */

typedef struct gc_vi_field {
    uint32_t xfb;                    /* dirección física del primer píxel */
    uint32_t width;                  /* píxeles por línea */
    uint32_t stride;                 /* bytes entre una línea y la siguiente */
    uint32_t height;                 /* líneas del campo */
    int      even;                   /* 0: campo impar (XFB de arriba), 1: par (de abajo) */
} gc_vi_field_t;

typedef void (*gc_vi_field_fn)(void *ctx, const gc_vi_field_t *field);

struct gc_system;

typedef struct gc_vi {
    struct gc_system *sys;
    uint16_t       reg[VI_REGS_SIZE / 2];
    uint32_t       half_line;        /* media línea actual del cuadro (0 = inicio del campo impar) */
    uint64_t       line_start;       /* ciclo en que empezó la línea actual */
    uint32_t       odd_first, odd_last, even_first, even_last;   /* medias líneas activas */
    uint32_t       next_si_poll;     /* media línea del próximo sondeo de mandos */
    uint64_t       fields;           /* campos terminados */
    gc_vi_field_t  last;             /* el último campo con XFB */
    gc_vi_field_fn on_field;
    void          *on_field_ctx;
} gc_vi_t;

/* Registro en el mapa de registros (una vez). */
void gc_vi_init(gc_vi_t *vi, struct gc_system *sys);

/* Valores de encendido (NTSC o PAL) y el primer evento de media línea. */
void gc_vi_reset(gc_vi_t *vi, int pal);

/* Ciclos de CPU por media línea con los registros actuales. */
uint32_t gc_vi_ticks_per_half_line(const gc_vi_t *vi);

/* Medias líneas de cada campo. */
uint32_t gc_vi_half_lines_odd(const gc_vi_t *vi);
uint32_t gc_vi_half_lines_even(const gc_vi_t *vi);

#endif
