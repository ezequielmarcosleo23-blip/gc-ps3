/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gc-ps3 - emulador de GameCube para PlayStation 3
 * Copyright (C) 2026 los autores de gc-ps3 (ver AUTHORS) */

/* Simulación en la PC (tests/appsim): las llamadas de lv2 para archivos. */
#ifndef APPSIM_SYS_FILE_H
#define APPSIM_SYS_FILE_H
#include <ppu-types.h>
#define SYS_O_RDONLY 0
typedef struct {
    s32 st_mode, st_uid, st_gid;
    s64 atime_, mtime_, ctime_;   /* en PSL1GHT: st_atime... (macros en glibc) */
    u64 st_size, st_blksize;
} sysFSStat;
typedef struct {
    u8 d_type, d_namlen;
    char d_name[256];
} sysFSDirent;
s32 sysLv2FsStat(const char *path, sysFSStat *st);
s32 sysLv2FsOpen(const char *path, s32 oflags, s32 *fd, u32 mode, const void *arg, u64 argsize);
s32 sysLv2FsRead(s32 fd, void *ptr, u64 len, u64 *read);
s32 sysLv2FsClose(s32 fd);
s32 sysLv2FsOpenDir(const char *path, s32 *fd);
s32 sysLv2FsReadDir(s32 fd, sysFSDirent *entry, u64 *read);
s32 sysLv2FsCloseDir(s32 fd);
#endif
