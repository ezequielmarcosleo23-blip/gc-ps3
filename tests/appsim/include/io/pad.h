/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS) */

/* Simulación en la PC (tests/appsim): lo que usa la app de io/pad.h. */
#ifndef APPSIM_IO_PAD_H
#define APPSIM_IO_PAD_H
#include <ppu-types.h>
#define MAX_PADS 127
typedef struct {
    u32 max, connected, info;
    u16 vendor_id[MAX_PADS];
    u16 product_id[MAX_PADS];
    u8 status[MAX_PADS];
} padInfo;
typedef struct {
    s32 len;
    u16 button[64];               /* [2]: DIGITAL1, [3]: DIGITAL2 */
} padData;
s32 ioPadInit(u32 max);
s32 ioPadEnd(void);
s32 ioPadGetInfo(padInfo *info);
s32 ioPadGetData(u32 port, padData *data);
#endif
