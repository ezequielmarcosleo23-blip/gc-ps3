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
 * Interfaz del DSP (0x0C005000), sin el DSP: registro de control, buzones,
 * DMA de la ARAM y DMA de audio.
 *
 * Como Source/Core/Core/HW/DSP.cpp de Dolphin (commit cf5cb7ef) con lo que
 * hace su DSP por HLE antes de cargar un microcódigo (DSPHLE.cpp, MailHandler,
 * los microcódigos ROM e INIT): al encender queda detenido con DSPInit en 1;
 * el reset del DSP deja el correo 0x8071FEED y bajar DSPInit deja 0x80544348.
 * Los correos de la CPU se toman al instante; los microcódigos de audio son de
 * una fase posterior (sin carga de microcódigo, sin mezcla).
 *
 * El DMA de la ARAM copia al instante y avisa al terminar, a los 246 ciclos
 * por bloque de 32 bytes. El DMA de audio avanza un bloque de 32 bytes cada
 * 8 muestras (121 392 ciclos a 32 kHz) y avisa (AID) al terminar la lista.
 */
#ifndef GCPS3_HW_DSP_H
#define GCPS3_HW_DSP_H

#include <stdint.h>

/* Registros (desplazamiento desde 0x0C005000, de 16 bits) */
enum {
    DSP_MAIL_TO_HI = 0x00, DSP_MAIL_TO_LO = 0x02, DSP_MAIL_FROM_HI = 0x04, DSP_MAIL_FROM_LO = 0x06,
    DSP_CSR = 0x0A, AR_INFO = 0x12, AR_MODE = 0x16, AR_REFRESH = 0x1A, AR_MMADDR_HI = 0x20,
    AR_MMADDR_LO = 0x22, AR_ARADDR_HI = 0x24, AR_ARADDR_LO = 0x26, AR_CNT_HI = 0x28,
    AR_CNT_LO = 0x2A, AUDIO_DMA_START_HI = 0x30, AUDIO_DMA_START_LO = 0x32,
    AUDIO_DMA_CONTROL = 0x36, AUDIO_DMA_LEFT = 0x3A,
};

/* DSPCSR */
#define DSP_CSR_RESET     0x0001u
#define DSP_CSR_ASSERTINT 0x0002u
#define DSP_CSR_HALT      0x0004u
#define DSP_CSR_AIDINT    0x0008u
#define DSP_CSR_AIDMASK   0x0010u
#define DSP_CSR_ARINT     0x0020u
#define DSP_CSR_ARMASK    0x0040u
#define DSP_CSR_DSPINT    0x0080u
#define DSP_CSR_DSPMASK   0x0100u
#define DSP_CSR_DMA       0x0200u           /* DMA de la ARAM en curso */
#define DSP_CSR_INITCODE  0x0400u
#define DSP_CSR_INIT      0x0800u

#define DSP_MAIL_ROM  0x8071FEEDu
#define DSP_MAIL_INIT 0x80544348u
#define DSP_MAIL_QUEUE 8

struct gc_system;

typedef struct gc_dsp {
    struct gc_system *sys;
    uint16_t csr;
    uint64_t initcode_clear_tb;            /* DSPInitCode vuelve a 0 en este tick */
    uint32_t cpu_mail;                     /* de la CPU al DSP */
    uint32_t mail[DSP_MAIL_QUEUE];         /* del DSP a la CPU */
    uint32_t mail_count;
    uint32_t last_mail;
    int      ucode;                        /* microcódigo por HLE: ROM, INIT o ninguno */
    uint32_t rom_param;                    /* microcódigo ROM: esperando un parámetro */
    uint16_t ar_info, ar_mode, ar_refresh;
    uint32_t ar_mm, ar_ar, ar_cnt;
    uint32_t audio_start, audio_current;
    uint16_t audio_control, audio_left;
    uint64_t audio_dma_blocks;             /* bloques de audio enviados (para pruebas) */
} gc_dsp_t;

void gc_dsp_init(gc_dsp_t *dsp, struct gc_system *sys);
void gc_dsp_reset(gc_dsp_t *dsp);

#endif
