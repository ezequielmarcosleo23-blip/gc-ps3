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
 * Prueba "rsx" (etapa C): el camino de video que usará el emulador, con rsxkit
 * (lib/rsxkit).
 *
 *   - Salida a 720p si la tele la da (rsxkit elige el modo).
 *   - Un destino de 640x480, el tamaño del EFB de la GameCube, con dos
 *     triángulos: uno de color fijo con el programa precompilado y otro con
 *     degradado y un fragment program generado en tiempo de ejecución.
 *   - El destino se presenta a 4:3 en la pantalla y encima va texto.
 *
 * Corre 10 s (600 cuadros) y sale sola. En el cuadro 60 lee píxeles del búfer
 * de color: OK si están los dos triángulos, el fondo del destino y el de la
 * pantalla. En RPCS3 la lectura necesita Configuration > GPU > Write Color
 * Buffers. Además se pide una captura de pantalla.
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <sysutil/sysutil.h>

#include <rsxkit/rsxkit.h>
#include <rsxkit/ui.h>

#include "common/harness.h"

#define EFB_W       640u
#define EFB_H       480u
#define FRAMES      600u
#define CHECK_FRAME 60u
#define WARMUP      10u
#define MAIN_BYTES  (1u << 20)

#define SCREEN_CLEAR 0x000000u
#define EFB_CLEAR    0x202020u
#define SOLID_RGB    0xFF3020u

static volatile int quit;

static void sysutil_cb(u64 status, u64 param, void *user)
{
    (void)param;
    (void)user;
    if (status == SYSUTIL_EXIT_GAME)
        quit = 1;
}

static void vtx(rsxkit_vertex *v, float x, float y, uint32_t rgb)
{
    memset(v, 0, sizeof *v);
    v->pos[0] = x;
    v->pos[1] = y;
    v->pos[2] = 0.5f;
    v->pos[3] = 1.0f;
    v->rgba[0] = (uint8_t)(rgb >> 16);
    v->rgba[1] = (uint8_t)(rgb >> 8);
    v->rgba[2] = (uint8_t)rgb;
    v->rgba[3] = 0xFF;
}

static int near_rgb(uint32_t got, uint32_t want)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((got >> s) & 0xFF) - (int)((want >> s) & 0xFF);
        if (d < -3 || d > 3)
            return 0;
    }
    return 1;
}

/* Píxel de la pantalla que corresponde al punto (x, y) del destino de 640x480. */
static uint32_t efb_pixel(const uint32_t *bb, uint32_t pitch, const rsxkit_rect *r, float x, float y)
{
    const uint32_t sx = (uint32_t)(r->x + (int32_t)(x * r->w / EFB_W));
    const uint32_t sy = (uint32_t)(r->y + (int32_t)(y * r->h / EFB_H));
    return bb[(size_t)sy * (pitch / 4) + sx] & 0x00FFFFFFu;
}

static int check_pixels(const rsxkit_screen *scr, const rsxkit_rect *r)
{
    uint32_t pitch = 0;
    const uint32_t *bb = rsxkit_back_buffer(&pitch);
    int ok = 1;

    const uint32_t solid = efb_pixel(bb, pitch, r, 190.0f, 307.0f);
    const uint32_t grad  = efb_pixel(bb, pitch, r, 450.0f, 307.0f);
    const uint32_t efb   = efb_pixel(bb, pitch, r, 320.0f, 40.0f);
    LOGI("pixeles: triangulo fijo %06x (esperado %06x), degradado %06x, fondo del destino %06x (esperado %06x)",
         (unsigned)solid, SOLID_RGB, (unsigned)grad, (unsigned)efb, EFB_CLEAR);
    ok &= near_rgb(solid, SOLID_RGB);
    ok &= !near_rgb(grad, EFB_CLEAR) && !near_rgb(grad, SOLID_RGB);
    ok &= near_rgb(efb, EFB_CLEAR);
    if (r->x > 8) {                                   /* franja negra a los lados del 4:3 */
        const uint32_t side = bb[4 * (pitch / 4) + 4] & 0x00FFFFFFu;
        LOGI("pixeles: franja lateral %06x (esperado %06x)", (unsigned)side, SCREEN_CLEAR);
        ok &= near_rgb(side, SCREEN_CLEAR);
    }
    (void)scr;
    return ok;
}

static int test_rsx(const ps3_env_t *env)
{
    (void)env;
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);

    const uint64_t ti = udplog_now_us();
    int rc = rsxkit_init(NULL);
    if (rc != RSXKIT_OK) {
        LOGE("rsxkit_init fallo (%d)", rc);
        return -1;
    }
    const rsxkit_screen *scr = rsxkit_screen_info();
    const int ui = rsxkit_ui_init();
    LOGI("rsxkit: pantalla %ux%u, aspecto %.3f, ui %d, init %llu us", (unsigned)scr->width,
         (unsigned)scr->height, (double)scr->aspect, ui, (unsigned long long)(udplog_now_us() - ti));
    if (scr->height != 720)
        LOGW("la salida no es 720p (la tele o RPCS3 no la ofrecen)");

    uint8_t *mem = memalign(1u << 20, MAIN_BYTES);
    uint32_t off = 0;
    if (!mem || rsxkit_map_main(mem, MAIN_BYTES, &off) != RSXKIT_OK) {
        LOGE("rsxkit_map_main fallo");
        rsxkit_shutdown();
        return -1;
    }
    rsxkit_target *efb = rsxkit_target_create(EFB_W, EFB_H);
    if (!efb) {
        LOGE("rsxkit_target_create(%u, %u) fallo", EFB_W, EFB_H);
        rsxkit_shutdown();
        return -1;
    }

    /* Vértices en memoria principal mapeada: el RSX los lee sin copia. */
    rsxkit_vertex *vb = (rsxkit_vertex *)mem;
    vtx(&vb[0], 80.0f, 400.0f, SOLID_RGB);
    vtx(&vb[1], 300.0f, 400.0f, SOLID_RGB);
    vtx(&vb[2], 190.0f, 120.0f, SOLID_RGB);
    vtx(&vb[3], 340.0f, 400.0f, 0xFF0000u);
    vtx(&vb[4], 560.0f, 400.0f, 0x00FF00u);
    vtx(&vb[5], 450.0f, 120.0f, 0x0000FFu);
    __sync_synchronize();

    /* Triángulo 1: combinación lineal fija (programa precompilado), color = vértice. */
    rsxkit_draw fixed;
    memset(&fixed, 0, sizeof fixed);
    fixed.blend = RSXKIT_BLEND_OFF;
    fixed.cull = RSXKIT_CULL_NONE;
    for (int c = 0; c < 4; c++)
        fixed.k[RSXKIT_K_ONE][1][c] = 1.0f;
    fixed.scissor[2] = EFB_W;
    fixed.scissor[3] = EFB_H;

    /* Triángulo 2: fragment program generado en tiempo de ejecución,
     * color = C * X0 con X0 = 1 (un término, un factor). */
    rsxkit_draw gen = fixed;
    memset(gen.k, 0, sizeof gen.k);
    gen.nterms = 1;
    gen.factors[0][0] = RSXKIT_FP_C;
    for (int c = 0; c < 4; c++)
        gen.k[0][0][c] = 1.0f;

    const rsxkit_view view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_NEAREST, 4.0f / 3.0f };
    rsxkit_rect shown = { 0, 0, 0, 0 };
    int checked = 0, pixels_ok = 0;
    uint64_t sum_us = 0, max_us = 0, prev = 0;
    uint32_t measured = 0, frame;
    const uint64_t vb0 = rsxkit_vblank_count();
    char line[96];

    for (frame = 0; frame < FRAMES && !quit; frame++) {
        sysUtilCheckCallback();

        rsxkit_frame_begin(SCREEN_CLEAR);
        const uint64_t now = udplog_now_us();
        if (frame > WARMUP) {
            const uint64_t dt = now - prev;
            sum_us += dt;
            if (dt > max_us)
                max_us = dt;
            measured++;
        }
        prev = now;

        rsxkit_bind(efb, NULL, (float)EFB_W, (float)EFB_H);
        rsxkit_clear(RSXKIT_CLEAR_COLOR, EFB_CLEAR, 0.0f, NULL);
        rsxkit_vertices(off, RSXKIT_MEM_MAIN);
        rsxkit_draw_triangles(&fixed, 0, 3);
        rsxkit_draw_triangles(&gen, 3, 3);
        rsxkit_target_present(efb, NULL, EFB_W, EFB_H, &view, &shown);

        if (frame == CHECK_FRAME) {
            rsxkit_finish();
            LOGI("destino 640x480 presentado en x=%d y=%d %ux%u", (int)shown.x, (int)shown.y,
                 (unsigned)shown.w, (unsigned)shown.h);
            pixels_ok = check_pixels(scr, &shown);
            checked = 1;
        }

        rsxkit_ui_begin(1280.0f, 720.0f);
        rsxkit_ui_rect(16, 16, 760, 132, 0x000000B0u);
        rsxkit_ui_text(32, 26, "gc-ps3 Fase 0 - prueba rsx", 2.0f, 0xFFFFFFFFu, 0);
        snprintf(line, sizeof line, "pantalla %ux%u, destino 640x480 (EFB) a 4:3",
                 (unsigned)scr->width, (unsigned)scr->height);
        rsxkit_ui_text(32, 66, line, 1.5f, 0xE0E0FFFFu, 0);
        snprintf(line, sizeof line, "cuadro %u de %u, %.2f ms por cuadro", frame, FRAMES,
                 measured ? sum_us / 1000.0 / measured : 0.0);
        rsxkit_ui_text(32, 92, line, 1.5f, 0xE0E0FFFFu, 0);
        rsxkit_ui_text(32, 118, "captura esta pantalla; sale sola en 10 s", 1.5f, 0xFFE080FFu, 0);
        rsxkit_ui_end();
        rsxkit_frame_end();
    }

    rsxkit_finish();
    const uint64_t vblanks = rsxkit_vblank_count() - vb0;
    rsxkit_fp_stats fps;
    rsxkit_fp_get_stats(&fps);
    LOGI("%u cuadros, %llu vblanks; por cuadro: media %.2f ms, max %.2f ms",
         frame, (unsigned long long)vblanks, measured ? sum_us / 1000.0 / measured : 0.0, max_us / 1000.0);
    LOGI("fragment programs generados: %u (en cache %u, cargas %u)", fps.generated, fps.programs, fps.loads);

    rsxkit_target_destroy(efb);
    rsxkit_shutdown();
    free(mem);
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);

    if (!checked) {
        LOGE("salida antes del cuadro %u: no se comprobaron los pixeles", CHECK_FRAME);
        return -1;
    }
    if (!pixels_ok) {
        LOGE("los pixeles no son los esperados (en RPCS3: activar Configuration > GPU > Write Color Buffers)");
        return -1;
    }
    if (fps.generated < 1) {
        LOGE("no se genero ningun fragment program");
        return -1;
    }
    LOGI("pixeles correctos");
    return 0;
}

int main(void)
{
    return phase0_main("rsx", test_rsx);
}
