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
 * La app de PS3 (src/platform/ps3/main.c) corriendo en la PC: versiones de
 * prueba de lo que usa de la PS3. `make test-app` la corre con un guion.
 *
 *   - Mando: el guion de la variable GUION, "cuadro:botones,..." con los
 *     botones en hexadecimal como los arma la app (DIGITAL1 | DIGITAL2 << 8).
 *     En vez de botones puede ir "xmb" (abrir y cerrar el menú del sistema:
 *     DRAW_BEGIN y DRAW_END) o "salir" (EXIT_GAME, como al cerrar el juego).
 *     Cada botón se aprieta un solo cuadro de la PS3.
 *   - Reloj: RELOJ fija la hora (segundos Unix); sin RELOJ, la de la PC.
 *     El reloj de microsegundos avanza 1 ms en cada lectura, así que cada
 *     cuadro de la PS3 corre lo mismo en cualquier máquina (15 tramos de la
 *     CPU) y el log sale siempre igual.
 *   - rsxkit: no dibuja. Cada subida de cuadro se pasa de nuevo a RGB y se
 *     anota su CRC-32 ("sim: subida ..." en stderr), que debe ser el del
 *     cuadro de la VI.
 *   - lv2: los archivos y carpetas con POSIX.
 *   - Log: a stdout, una línea por mensaje (como la TTY de RPCS3).
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ppu-types.h>
#include <io/pad.h>
#include <sys/file.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

#include <rsxkit/rsxkit.h>
#include <rsxkit/ui.h>

#include "core/crc32.h"
#include "platform/ps3/env.h"
#include "platform/ps3/udplog.h"

#define SCRIPT_MAX  128
#define FRAME_LIMIT 20000u          /* cuadros de la PS3: el guion se colgó */

enum { EV_BUTTONS, EV_XMB, EV_EXIT };

typedef struct event {
    unsigned frame, buttons;
    int kind;
} event_t;

static event_t script[SCRIPT_MAX];
static int nscript;
static unsigned frame;              /* cuadros de la PS3 terminados */
static unsigned uploads, draws, xmb_returns;
static uint64_t clock_us;

static sysutilCallback sysutil_fn;
static void *sysutil_user;

static void load_script(void)
{
    const char *s = getenv("GUION");
    while (s && *s && nscript < SCRIPT_MAX) {
        event_t *e = &script[nscript];
        char *end;
        e->frame = (unsigned)strtoul(s, &end, 10);
        if (end == s || *end != ':') {
            fprintf(stderr, "sim: GUION mal escrito en \"%s\"\n", s);
            exit(2);
        }
        s = end + 1;
        if (strncmp(s, "xmb", 3) == 0) {
            e->kind = EV_XMB;
            s += 3;
        } else if (strncmp(s, "salir", 5) == 0) {
            e->kind = EV_EXIT;
            s += 5;
        } else {
            e->kind = EV_BUTTONS;
            e->buttons = (unsigned)strtoul(s, &end, 16);
            if (end == s) {
                fprintf(stderr, "sim: GUION mal escrito en \"%s\"\n", s);
                exit(2);
            }
            s = end;
        }
        nscript++;
        while (*s == ',')
            s++;
    }
}

/* ---- mando --------------------------------------------------------------------------- */

s32 ioPadInit(u32 max)
{
    (void)max;
    load_script();
    return 0;
}

s32 ioPadEnd(void) { return 0; }

s32 ioPadGetInfo(padInfo *info)
{
    memset(info, 0, sizeof *info);
    info->max = 7;
    info->connected = 1;
    info->status[0] = 1;
    return 0;
}

s32 ioPadGetData(u32 port, padData *data)
{
    (void)port;
    memset(data, 0, sizeof *data);
    unsigned b = 0;
    for (int k = 0; k < nscript; k++)
        if (script[k].kind == EV_BUTTONS && script[k].frame == frame)
            b = script[k].buttons;
    data->len = 24;
    data->button[2] = (u16)(b & 0xFF);
    data->button[3] = (u16)(b >> 8);
    return 0;
}

/* ---- sistema ------------------------------------------------------------------------- */

s32 sysGetCurrentTime(u64 *sec, u64 *nsec)
{
    const char *fixed = getenv("RELOJ");
    *sec = fixed ? (u64)strtoull(fixed, NULL, 10) : (u64)time(NULL);
    *nsec = 0;
    return 0;
}

s32 sysUtilRegisterCallback(s32 slot, sysutilCallback cb, void *user)
{
    (void)slot;
    sysutil_fn = cb;
    sysutil_user = user;
    return 0;
}

s32 sysUtilUnregisterCallback(s32 slot)
{
    (void)slot;
    sysutil_fn = NULL;
    return 0;
}

s32 sysUtilCheckCallback(void)
{
    for (int k = 0; k < nscript && sysutil_fn; k++) {
        if (script[k].frame != frame)
            continue;
        if (script[k].kind == EV_XMB) {
            sysutil_fn(SYSUTIL_DRAW_BEGIN, 0, sysutil_user);
            sysutil_fn(SYSUTIL_DRAW_END, 0, sysutil_user);
        } else if (script[k].kind == EV_EXIT) {
            sysutil_fn(SYSUTIL_EXIT_GAME, 0, sysutil_user);
        }
    }
    return 0;
}

void ps3_env_detect(ps3_env_t *env) { memset(env, 0, sizeof *env); }

/* ---- archivos (lv2) ------------------------------------------------------------------ */

s32 sysLv2FsStat(const char *path, sysFSStat *st)
{
    struct stat s;
    if (stat(path, &s) != 0)
        return -1;
    memset(st, 0, sizeof *st);
    st->st_mode = (s32)s.st_mode;
    st->st_size = (u64)s.st_size;
    return 0;
}

s32 sysLv2FsOpen(const char *path, s32 oflags, s32 *fd, u32 mode, const void *arg, u64 argsize)
{
    (void)oflags;
    (void)mode;
    (void)arg;
    (void)argsize;
    const int f = open(path, O_RDONLY);
    if (f < 0)
        return -1;
    *fd = f;
    return 0;
}

s32 sysLv2FsRead(s32 fd, void *ptr, u64 len, u64 *done)
{
    const ssize_t n = read(fd, ptr, (size_t)len);
    if (n < 0)
        return -1;
    *done = (u64)n;
    return 0;
}

s32 sysLv2FsClose(s32 fd) { return close(fd) == 0 ? 0 : -1; }

#define DIRS_MAX 8
static DIR *dirs[DIRS_MAX];

s32 sysLv2FsOpenDir(const char *path, s32 *fd)
{
    for (int k = 0; k < DIRS_MAX; k++) {
        if (dirs[k])
            continue;
        if (!(dirs[k] = opendir(path)))
            return -1;
        *fd = k;
        return 0;
    }
    return -1;
}

s32 sysLv2FsReadDir(s32 fd, sysFSDirent *entry, u64 *done)
{
    const struct dirent *d = readdir(dirs[fd]);
    *done = 0;
    if (!d)
        return 0;                   /* como lv2: sin error y 0 bytes al final */
    memset(entry, 0, sizeof *entry);
    snprintf(entry->d_name, sizeof entry->d_name, "%s", d->d_name);
    entry->d_namlen = (u8)strlen(entry->d_name);
    *done = sizeof *entry;
    return 0;
}

s32 sysLv2FsCloseDir(s32 fd)
{
    closedir(dirs[fd]);
    dirs[fd] = NULL;
    return 0;
}

/* ---- rsxkit -------------------------------------------------------------------------- */

struct rsxkit_fb {
    uint32_t max_w, max_h;
};

static const rsxkit_screen screen = { 1280, 720, 16.0f / 9.0f };
static struct rsxkit_fb the_fb;
static int fb_alive;

int rsxkit_init(const rsxkit_config *cfg)
{
    (void)cfg;
    return RSXKIT_OK;
}

void rsxkit_shutdown(void) {}
void rsxkit_finish(void) {}
const rsxkit_screen *rsxkit_screen_info(void) { return &screen; }
void rsxkit_frame_begin(uint32_t clear_rgb) { (void)clear_rgb; }

void rsxkit_frame_end(void)
{
    if (++frame > FRAME_LIMIT) {
        fprintf(stderr, "sim: %u cuadros de la PS3 y el guion no terminó\n", FRAME_LIMIT);
        exit(3);
    }
}

int rsxkit_after_system(void)
{
    xmb_returns++;
    return 0;
}

rsxkit_fb *rsxkit_fb_create(uint32_t max_width, uint32_t max_height)
{
    the_fb.max_w = max_width;
    the_fb.max_h = max_height;
    fb_alive = 1;
    return &the_fb;
}

void rsxkit_fb_destroy(rsxkit_fb *fb)
{
    if (fb == &the_fb)
        fb_alive = 0;
}

int rsxkit_fb_upload(rsxkit_fb *fb, const void *pixels, uint32_t width, uint32_t height,
                     uint32_t pitch, rsxkit_format format)
{
    if (fb != &the_fb || !fb_alive || width > fb->max_w || height > fb->max_h ||
        format != RSXKIT_XRGB8888 || pitch < width * 4) {
        fprintf(stderr, "sim: subida inválida %ux%u pitch %u formato %d\n", (unsigned)width,
                (unsigned)height, (unsigned)pitch, (int)format);
        exit(4);
    }
    /* De vuelta a RGB, para comparar con el CRC del cuadro de la VI */
    uint32_t crc = 0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *p = (const uint8_t *)pixels + (size_t)y * pitch;
        for (uint32_t x = 0; x < width; x++, p += 4)
            crc = gc_crc32(crc, p + 1, 3);
    }
    uploads++;
    fprintf(stderr, "sim: subida %u %ux%u, crc32 RGB 0x%08x\n", uploads, (unsigned)width,
            (unsigned)height, (unsigned)crc);
    return 0;
}

void rsxkit_fb_draw(rsxkit_fb *fb, const rsxkit_view *view, rsxkit_rect *rect)
{
    (void)view;
    (void)rect;
    if (fb == &the_fb && fb_alive)
        draws++;
}

int rsxkit_ui_init(void) { return 0; }
void rsxkit_ui_begin(float lw, float lh) { (void)lw; (void)lh; }
void rsxkit_ui_end(void) {}

void rsxkit_ui_gradient(float x, float y, float w, float h, uint32_t tl, uint32_t tr,
                        uint32_t bl, uint32_t br)
{
    (void)x; (void)y; (void)w; (void)h; (void)tl; (void)tr; (void)bl; (void)br;
}

float rsxkit_ui_text(float x, float y, const char *utf8, float scale, uint32_t rgba,
                     uint32_t max_cells)
{
    (void)x; (void)scale; (void)rgba; (void)max_cells;
    if (getenv("TEXTO") && frame % 100 == 0)
        fprintf(stderr, "sim: [%u] y=%.0f %s\n", frame, (double)y, utf8);
    return 0.0f;
}

/* ---- log ----------------------------------------------------------------------------- */

int udplog_init(const char *test_name)
{
    (void)test_name;
    return 0;
}

void udplog_printf(char level, const char *fmt, ...)
{
    va_list ap;
    if (level == 'W' || level == 'E')
        printf("[%c] ", level);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

void udplog_shutdown(void)
{
    fprintf(stderr, "sim: %u cuadros de la PS3, %u subidas, %u dibujos, %u vueltas del XMB\n",
            frame, uploads, draws, xmb_returns);
    fflush(stdout);
}

uint64_t udplog_now_us(void) { return clock_us += 1000; }

const char *udplog_targets(void) { return "(simulado en la PC)"; }
