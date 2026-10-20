/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS) */

/* Simulación en la PC (tests/appsim). */
#ifndef APPSIM_SYSUTIL_SYSUTIL_H
#define APPSIM_SYSUTIL_SYSUTIL_H
#include <ppu-types.h>
#define SYSUTIL_EVENT_SLOT0 0
#define SYSUTIL_EXIT_GAME   0x0101
#define SYSUTIL_DRAW_BEGIN  0x0121
#define SYSUTIL_DRAW_END    0x0122
typedef void (*sysutilCallback)(u64 status, u64 param, void *usrdata);
s32 sysUtilRegisterCallback(s32 slot, sysutilCallback cb, void *usrdata);
s32 sysUtilUnregisterCallback(s32 slot);
s32 sysUtilCheckCallback(void);
#endif
