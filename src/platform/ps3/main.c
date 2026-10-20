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
 * La app de PS3: un menú con los DOLs y el núcleo corriendo el elegido.
 *
 *   - DOLs: los que trae la app (USRDIR/dol, o "dol" al lado del .self) y
 *     los de /dev_hdd0/gcps3/dol. Cruz corre el elegido; triángulo sale.
 *   - Corriendo: el núcleo avanza por tramos de hasta 15 ms por cuadro de la
 *     PS3; cada cuadro de la VI se pasa a RGB en el PPU (core/xfb.c, el mismo
 *     código del runner) y se muestra a 4:3 con rsxkit. Círculo vuelve al menú.
 *   - Log por UDP y TTY: lo que imprime el DOL tal cual; los mensajes de la
 *     app empiezan con "app: " y los del núcleo con "emu: ". Cada cuadro
 *     distinto sale como "app: cuadro N (campo F) WxH, crc32 RGB 0x...", igual
 *     que "runner --cuadros". Al empezar se anota el reloj y al terminar los
 *     ciclos corridos: tools/compare_log.py corre el runner igual y compara.
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <io/pad.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

#include <rsxkit/rsxkit.h>
#include <rsxkit/ui.h>

#include "core/frames.h"
#include "core/log.h"
#include "core/system.h"
#include "core/version.h"
#include "platform/ps3/env.h"
#include "platform/ps3/files.h"
#include "platform/ps3/paths.h"
#include "platform/ps3/udplog.h"

#define LOGI(...) udplog_printf('I', __VA_ARGS__)
#define LOGW(...) udplog_printf('W', __VA_ARGS__)
#define LOGE(...) udplog_printf('E', __VA_ARGS__)

#define MAX_DOLS        64
#define DOL_MAX         (32u << 20)
#define MAP_MAX         (4u << 20)
#define FRAME_MAX_W     720u              /* los modos de video normales entran */
#define FRAME_MAX_H     576u
#define SLICE_CYCLES    200000u           /* tramo de CPU entre vistazos al reloj */
#define BUDGET_US       15000u            /* emulación por cuadro de la PS3 */
#define UI_W            1280.0f
#define UI_H            720.0f

/* Botones: DIGITAL1 en el byte bajo, DIGITAL2 en el alto (io/pad.h) */
enum {
    BTN_START = 1u << 3, BTN_UP = 1u << 4, BTN_DOWN = 1u << 6,
    BTN_TRIANGLE = 1u << 12, BTN_CIRCLE = 1u << 13, BTN_CROSS = 1u << 14,
};

typedef struct app {
    ps3_dol_t     dols[MAX_DOLS];
    int           ndols, cursor;
    int           running, ended;
    char          name[PS3_NAME_MAX];
    uint64_t      cycles, busy_us, started_us;
    uint8_t      *rgb, *xrgb;
    rsxkit_fb    *fb;
    int           image;                  /* hay un cuadro subido */
    int           upload;                 /* hay un cuadro nuevo para subir */
    uint32_t      img_w, img_h;
    gc_frames_t   frames;
} app_t;

static gc_system_t sys;
static app_t app;
static volatile int quit, after_system;

static void sysutil_cb(u64 status, u64 param, void *user)
{
    (void)param;
    (void)user;
    if (status == SYSUTIL_EXIT_GAME)
        quit = 1;
    else if (status == SYSUTIL_DRAW_END)
        after_system = 1;
}

static void core_sink(char level, const char *msg)
{
    udplog_printf(level, "emu: %s", msg);
}

static void report_line(void *ctx, const char *line)
{
    (void)ctx;
    udplog_printf('I', "%s", line);              /* la salida del DOL, sin prefijo */
}

static void frame_seen(void *ctx, const gc_frame_t *f)
{
    app_t *a = ctx;
    LOGI("app: cuadro %u (campo %llu) %ux%u, crc32 RGB 0x%08x", (unsigned)f->number,
         (unsigned long long)f->field, (unsigned)f->width, (unsigned)f->height, (unsigned)f->crc);
    a->upload = 1;
}

static uint64_t now_us(void) { return udplog_now_us(); }

static uint64_t rtc_now(void)
{
    u64 sec = 0, nsec = 0;
    sysGetCurrentTime(&sec, &nsec);
    return sec > GC_EPOCH_UNIX ? sec - GC_EPOCH_UNIX : 0;
}

/* Botones del primer mando conectado (ioPadGetData no repite si no cambió) */
static uint32_t read_pad(void)
{
    static uint32_t held;
    padInfo info;
    if (ioPadGetInfo(&info) != 0)
        return held;
    for (u32 p = 0; p < MAX_PADS; p++) {
        if (!info.status[p])
            continue;
        padData d;
        if (ioPadGetData(p, &d) == 0 && d.len > 0)
            held = (d.button[2] & 0xFFu) | (uint32_t)(d.button[3] & 0xFFu) << 8;
        return held;
    }
    held = 0;
    return 0;
}

/* ---- correr un DOL ------------------------------------------------------------------ */

static int start(app_t *a, const ps3_dol_t *d)
{
    uint8_t *dol = NULL, *map = NULL;
    size_t len = 0, map_len = 0;
    char map_path[PS3_PATH_MAX];

    if (ps3_read_file(d->path, &dol, &len, DOL_MAX) != 0) {
        LOGE("app: no se pudo leer %s", d->path);
        return -1;
    }
    const gc_boot_config_t cfg = { 0, rtc_now() };
    gc_frames_reset(&a->frames);
    const gc_dol_error_t err = gc_system_boot_dol(&sys, dol, len, &cfg);
    free(dol);
    if (err != GC_DOL_OK) {
        LOGE("app: %s: %s", d->name, gc_dol_strerror(err));
        return -1;
    }
    ps3_map_path(d->path, map_path, sizeof map_path);
    if (ps3_read_file(map_path, &map, &map_len, MAP_MAX) == 0) {
        gc_system_load_map(&sys, (const char *)map, map_len);
        free(map);
    }
    sys.hle.report = report_line;

    snprintf(a->name, sizeof a->name, "%s", d->name);
    a->running = 1;
    a->ended = 0;
    a->image = a->upload = 0;
    a->cycles = a->busy_us = 0;
    a->started_us = now_us();
    /* El reloj que quedó en la GameCube emulada: compare_log arranca el runner igual */
    LOGI("app: corriendo %s (reloj %llu)", d->name, (unsigned long long)sys.rtc_boot);
    return 0;
}

static void stop(app_t *a)
{
    gc_hle_flush(&sys);
    LOGI("app: fin de %s, %llu ciclos, %llu campos, %u cuadros distintos", a->name,
         (unsigned long long)a->cycles, (unsigned long long)sys.vi.fields,
         (unsigned)a->frames.last.number);
    a->running = 0;
}

/* Un tramo de emulación: hasta dos campos nuevos o BUDGET_US */
static void emulate(app_t *a)
{
    if (a->ended)
        return;
    const uint64_t t0 = now_us(), f0 = sys.vi.fields;
    for (;;) {
        const uint64_t ran = gc_system_run(&sys, SLICE_CYCLES);
        a->cycles += ran;
        if (sys.cpu.stop || sys.hle.exited || ran == 0) {
            gc_hle_flush(&sys);
            a->ended = 1;
            if (sys.hle.exited)
                LOGI("app: %s termino con exit(%d)", a->name, (int)sys.hle.exit_code);
            else
                LOGW("app: %s se detuvo en 0x%08x", a->name, (unsigned)sys.cpu.pc);
            break;
        }
        if (sys.vi.fields - f0 >= 2 || now_us() - t0 >= BUDGET_US)
            break;
    }
    a->busy_us += now_us() - t0;
}

/* El último cuadro avisado, de RGB a XRGB8888 para rsxkit */
static void upload(app_t *a)
{
    const gc_frame_t *f = &a->frames.last;
    a->upload = 0;
    if (!f->number || f->width > FRAME_MAX_W || f->height > FRAME_MAX_H)
        return;
    const size_t n = (size_t)f->width * f->height;
    const uint8_t *s = f->rgb;
    uint8_t *d = a->xrgb;
    for (size_t k = 0; k < n; k++, s += 3, d += 4) {
        d[0] = 0xFF;
        d[1] = s[0];
        d[2] = s[1];
        d[3] = s[2];
    }
    if (rsxkit_fb_upload(a->fb, a->xrgb, f->width, f->height, f->width * 4, RSXKIT_XRGB8888) == 0) {
        a->image = 1;
        a->img_w = f->width;
        a->img_h = f->height;
    }
}

/* ---- pantalla ----------------------------------------------------------------------- */

static void draw_menu(const app_t *a)
{
    char line[128];
    rsxkit_ui_gradient(0, 0, UI_W, UI_H, 0x101830FFu, 0x101830FFu, 0x000008FFu, 0x000008FFu);
    snprintf(line, sizeof line, "%s %s", GCPS3_NAME, GCPS3_VERSION);
    rsxkit_ui_text(80, 50, line, 3.0f, 0xFFFFFFFFu, 0);
    rsxkit_ui_text(80, 100, "Elegí un programa de GameCube (DOL)", 2.0f, 0xC0D0FFFFu, 0);
    if (a->ndols == 0) {
        rsxkit_ui_text(80, 180, "No hay DOLs: copialos a /dev_hdd0/gcps3/dol", 2.0f, 0xFFE080FFu, 0);
    }
    const int rows = 14, first = a->cursor >= rows ? a->cursor - rows + 1 : 0;
    for (int k = first; k < a->ndols && k < first + rows; k++) {
        const float y = 160.0f + 34.0f * (float)(k - first);
        if (k == a->cursor)
            rsxkit_ui_rect(70, y - 4, 820, 32, 0x3050A0C0u);
        snprintf(line, sizeof line, "%s%s", a->dols[k].name, a->dols[k].bundled ? "" : "  (hdd)");
        rsxkit_ui_text(90, y, line, 2.0f, k == a->cursor ? 0xFFFFFFFFu : 0xB0B8D0FFu, 40);
    }
    rsxkit_ui_text(80, 660, "Cruz: correr    Triángulo: salir", 2.0f, 0x90A0C0FFu, 0);
}

static void draw_status(const app_t *a)
{
    char line[160];
    const double mips = a->busy_us ? (double)a->cycles / (double)a->busy_us : 0.0;
    rsxkit_ui_rect(16, 16, 900, 70, 0x000000A0u);
    snprintf(line, sizeof line, "%s   campo %llu   %.1f MIPS (%.0f%% de la GameCube)", a->name,
             (unsigned long long)sys.vi.fields, mips, mips / 486.0 * 100.0);
    rsxkit_ui_text(28, 24, line, 1.5f, 0xFFFFFFFFu, 0);
    if (a->ended && sys.hle.exited)
        snprintf(line, sizeof line, "terminó con exit(%d)   Círculo: menú", (int)sys.hle.exit_code);
    else if (a->ended)
        snprintf(line, sizeof line, "detenido en 0x%08x   Círculo: menú", (unsigned)sys.cpu.pc);
    else if (!a->image)
        snprintf(line, sizeof line, "sin imagen todavía   Círculo: menú");
    else
        snprintf(line, sizeof line, "Círculo: menú");
    rsxkit_ui_text(28, 52, line, 1.5f, 0xFFE080FFu, 0);
}

/* ---- arranque --------------------------------------------------------------------- */

#ifndef GCPS3_APPID
#define GCPS3_APPID "GCPS00001"
#endif

/* Las carpetas de DOLs: "dol" al lado del ejecutable (el .self en RPCS3), la
 * del pkg instalado (USRDIR/dol) y la de datos del disco */
static void scan(app_t *a, const char *argv0)
{
    char dirs[3][PS3_PATH_MAX];
    int n = 0;
    a->ndols = a->cursor = 0;
    const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (slash)
        snprintf(dirs[n++], PS3_PATH_MAX, "%.*s/dol", (int)(slash - argv0), argv0);
    snprintf(dirs[n++], PS3_PATH_MAX, "/dev_hdd0/game/%s/USRDIR/dol", GCPS3_APPID);
    snprintf(dirs[n++], PS3_PATH_MAX, "%s/dol", GCPS3_DATA_DIR);
    for (int k = 0; k < n; k++) {
        int seen = 0;
        for (int j = 0; j < k; j++)
            seen |= strcmp(dirs[j], dirs[k]) == 0;
        if (!seen)
            LOGI("app: %d DOLs en %s", ps3_list_dols(dirs[k], k < n - 1, a->dols, &a->ndols, MAX_DOLS),
                 dirs[k]);
    }
}

int main(int argc, char **argv)
{
    ps3_env_t env;
    app_t *a = &app;

    udplog_init("gc-ps3");
    gc_log_set_sink(core_sink);
    LOGI("app: %s %s (compilada %s %s)", GCPS3_NAME, GCPS3_VERSION, __DATE__, __TIME__);
    LOGI("app: log UDP %s", udplog_targets());
    ps3_env_detect(&env);
    LOGI("app: entorno %s", env.is_hen ? "HEN" : env.is_mamba ? "Mamba" : env.is_cobra ? "Cobra" : "sin CFW/HEN");

    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
    ioPadInit(7);
    int rc = rsxkit_init(NULL);
    if (rc != RSXKIT_OK) {
        LOGE("app: rsxkit_init fallo (%d)", rc);
        udplog_shutdown();
        return 1;
    }
    rsxkit_ui_init();
    const rsxkit_screen *scr = rsxkit_screen_info();
    LOGI("app: pantalla %ux%u", (unsigned)scr->width, (unsigned)scr->height);

    a->fb = rsxkit_fb_create(FRAME_MAX_W, FRAME_MAX_H);
    a->rgb = malloc(FRAME_MAX_W * FRAME_MAX_H * 3);
    a->xrgb = memalign(128, FRAME_MAX_W * FRAME_MAX_H * 4);
    if (!a->fb || !a->rgb || !a->xrgb || gc_system_init(&sys) != 0) {
        LOGE("app: sin memoria para la GameCube emulada");
        rsxkit_shutdown();
        udplog_shutdown();
        return 1;
    }
    gc_frames_attach(&a->frames, &sys, a->rgb, FRAME_MAX_W * FRAME_MAX_H * 3, frame_seen, a);
    scan(a, argc > 0 ? argv[0] : NULL);

    const rsxkit_view view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f };
    uint32_t prev = 0;
    while (!quit) {
        sysUtilCheckCallback();
        if (after_system) {
            after_system = 0;
            rsxkit_after_system();
        }
        const uint32_t held = read_pad(), pressed = held & ~prev;
        prev = held;

        if (!a->running) {
            if ((pressed & BTN_UP) && a->cursor > 0)
                a->cursor--;
            if ((pressed & BTN_DOWN) && a->cursor + 1 < a->ndols)
                a->cursor++;
            if ((pressed & BTN_CROSS) && a->ndols > 0)
                start(a, &a->dols[a->cursor]);
            if (pressed & BTN_TRIANGLE)
                quit = 1;
        } else {
            if (pressed & BTN_CIRCLE)
                stop(a);
            else
                emulate(a);
            if (a->upload)
                upload(a);
        }

        rsxkit_frame_begin(0x000000);
        if (a->running && a->image)
            rsxkit_fb_draw(a->fb, &view, NULL);
        rsxkit_ui_begin(UI_W, UI_H);
        if (a->running)
            draw_status(a);
        else
            draw_menu(a);
        rsxkit_ui_end();
        rsxkit_frame_end();
    }

    if (a->running)
        stop(a);
    LOGI("app: salida");
    rsxkit_finish();
    rsxkit_fb_destroy(a->fb);
    rsxkit_shutdown();
    ioPadEnd();
    gc_system_free(&sys);
    free(a->rgb);
    free(a->xrgb);
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    udplog_shutdown();
    return 0;
}
