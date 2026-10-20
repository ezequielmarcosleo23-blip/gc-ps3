/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS) */

/* Simulación en la PC (tests/appsim). */
#ifndef APPSIM_SYS_SYSTIME_H
#define APPSIM_SYS_SYSTIME_H
#include <ppu-types.h>
s32 sysGetCurrentTime(u64 *sec, u64 *nsec);
#endif
