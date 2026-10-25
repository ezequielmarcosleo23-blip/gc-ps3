/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Runner en PC: arranca un DOL por HLE y lo corre en el intérprete de Gekko,
 * sin ventana: al final informa el último cuadro de la VI (tamaño y CRC-32 de
 * sus bytes RGB) y, si se pide, lo guarda como PNG.
 *
 *   gc-ps3-runner [opciones] programa.dol
 *     --ciclos N       cuántos ciclos de CPU correr (por omisión 486 000 000 = 1 s)
 *     --map ARCHIVO    símbolos para los ganchos de depuración (formato de Dolphin;
 *                      por omisión, el .map al lado del DOL si existe)
 *     --traza ARCHIVO  una línea por instrucción: "pc instrucción" en hexadecimal
 *     --pal            el IPL dejó modo PAL (por omisión NTSC)
 *     --rtc SEGUNDOS   reloj al arrancar, en segundos desde el 2000-01-01
 *                      (por omisión, la hora de la PC)
 *     --imagen ARCHIVO el último cuadro de la VI en PNG (640x480 en 480i)
 *     --cuadros        cada cuadro distinto que muestra la VI (al terminar un
 *                      campo): número, campo, tamaño y CRC-32 de sus bytes RGB.
 *                      Es la misma lista que da la app de PS3 (tools/compare_log.py)
 *     --huella ARCHIVO pc, instrucción y huella del estado de la CPU antes de
 *                      cada instrucción, en binario (16 bytes por instrucción;
 *                      docs/traza-dolphin.md). Lo mismo que el Dolphin parchado
 *     --detalle D:H    con --huella: el estado completo de las instrucciones
 *                      D a H-1 en ARCHIVO.detalle
 *     --puntos ARCHIVO la huella encadenada cada 65 536 instrucciones y al final
 *     --estado-gx ARCHIVO  al terminar, el estado de GX en texto (FIFO, BP, CP, XF)
 *     --vertices ARCHIVO   cada vértice transformado, en texto, como el volcado
 *                      del Dolphin parchado (GCPS3_VERTICES)
 *     --triangulos ARCHIVO cada triángulo que llega al rasterizador (GCPS3_TRIANGULOS)
 *     --copias ARCHIVO     cada copia del EFB y al final el EFB, con CRC (GCPS3_COPIAS)
 *     --efb ARCHIVO        al terminar, el color del EFB en PNG (640x528, con el
 *                      formato de píxel de ese momento): para mirar lo dibujado
 *     --gx-lotes       el rasterizador junta los triángulos y dibuja al vaciar
 *                      el lote, como un backend con lotes (tiene que dar lo
 *                      mismo); al final, cuántos lotes hubo
 *
 * Lo que el programa imprime (OSReport, printf...) sale por la salida estándar;
 * los mensajes del emulador, por la de error. Sale con el código de exit() del
 * programa (o 0 si se acabaron los ciclos), 1 si no se pudo cargar y 2 si las
 * opciones están mal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/crc32.h"
#include "core/frames.h"
#include "core/log.h"
#include "core/png.h"
#include "core/system.h"
#include "core/version.h"
#include "core/xfb.h"
#include "cpu/huella.h"
#include "gx/raster.h"

#define DEFAULT_CYCLES GC_CPU_CLOCK
#define MAX_FILE       (32u << 20)

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "uso: %s [opciones] programa.dol\n"
            "  --ciclos N       ciclos de CPU a correr (por omision %u, 1 s)\n"
            "  --map ARCHIVO    simbolos para los ganchos (por omision, el .map del DOL)\n"
            "  --traza ARCHIVO  pc e instruccion de cada paso\n"
            "  --pal            modo de video PAL (por omision NTSC)\n"
            "  --rtc SEGUNDOS   reloj al arrancar, segundos desde el 2000-01-01\n"
            "  --imagen ARCHIVO el ultimo cuadro de la VI en PNG\n"
            "  --cuadros        cada cuadro distinto de la VI con su CRC\n"
            "  --huella ARCHIVO pc, instruccion y huella del estado de cada paso (binario)\n"
            "  --detalle D:H    con --huella: estado completo de los pasos D a H-1\n"
            "  --puntos ARCHIVO huella encadenada cada 65536 pasos y al final\n"
            "  --estado-gx ARCHIVO  al terminar, el estado de GX en texto (FIFO, BP, CP, XF)\n"
            "  --vertices ARCHIVO   cada vertice transformado, en texto (como Dolphin)\n"
            "  --triangulos ARCHIVO cada triangulo que llega al rasterizador (como Dolphin)\n"
            "  --copias ARCHIVO     cada copia del EFB y el EFB al final, con CRC (como Dolphin)\n"
            "  --efb ARCHIVO        al terminar, el color del EFB en PNG\n"
            "  --gx-lotes           dibujar por lotes (tiene que dar lo mismo) y contarlos\n"
            "  --version, --help\n",
            argv0, (unsigned)DEFAULT_CYCLES);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = malloc(MAX_FILE);
    size_t n = buf ? fread(buf, 1, MAX_FILE, f) : 0;
    const int too_big = buf && n == MAX_FILE && fgetc(f) != EOF;
    fclose(f);
    if (!buf || too_big) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static gc_system_t sys;
static FILE *trace_file;

/* --huella, --detalle y --puntos (docs/traza-dolphin.md) */
#define PUNTOS_CADA 65536u

typedef struct huella_out {
    FILE     *huella, *detalle, *puntos;
    uint64_t  desde, hasta;               /* --detalle */
    uint64_t  n;                          /* instrucciones vistas */
    uint64_t  cadena;
    uint32_t  last_pc;
} huella_out_t;

static huella_out_t hout;

static void put_le(uint8_t *p, uint64_t v, int bytes)
{
    for (int k = 0; k < bytes; k++)
        p[k] = (uint8_t)(v >> (8 * k));
}

static void huella_step(uint32_t pc, uint32_t inst)
{
    const uint64_t estado = gekko_huella(&sys.cpu);
    if (hout.huella) {
        uint8_t rec[16];
        put_le(rec, pc, 4);
        put_le(rec + 4, inst, 4);
        put_le(rec + 8, estado, 8);
        fwrite(rec, 1, sizeof rec, hout.huella);
    }
    if (hout.detalle && hout.n >= hout.desde && hout.n < hout.hasta) {
        uint64_t w[GEKKO_ESTADO_PALABRAS];
        uint8_t rec[16 + 8 * GEKKO_ESTADO_PALABRAS];
        gekko_estado(&sys.cpu, w);
        put_le(rec, hout.n, 8);
        put_le(rec + 8, pc, 4);
        put_le(rec + 12, inst, 4);
        for (int k = 0; k < GEKKO_ESTADO_PALABRAS; k++)
            put_le(rec + 16 + 8 * k, w[k], 8);
        fwrite(rec, 1, sizeof rec, hout.detalle);
    }
    hout.cadena = gekko_huella_cadena(hout.cadena, pc, inst, estado);
    hout.last_pc = pc;
    hout.n++;
    if (hout.puntos && hout.n % PUNTOS_CADA == 0)
        fprintf(hout.puntos, "%llu %08x %016llx\n", (unsigned long long)hout.n, (unsigned)pc,
                (unsigned long long)hout.cadena);
}

/* La traza muestra la instrucción original también donde hay un gancho. */
static void trace_line(void *ctx, uint32_t pc, uint32_t inst)
{
    (void)ctx;
    inst = gc_hle_original(&sys, inst);
    if (trace_file)
        fprintf(trace_file, "%08x %08x\n", (unsigned)pc, (unsigned)inst);
    if (hout.huella || hout.puntos)
        huella_step(pc, inst);
}

static void estado_line(void *ctx, const char *text)
{
    fprintf(ctx, "%s\n", text);
}

/* --estado-gx: como Dolphin al cortar la traza, con los eventos de este ciclo
 * ya atendidos (docs/traza-dolphin.md). -1 si no se pudo escribir. */
static int write_estado_gx(const char *path)
{
    gc_sched_dispatch(&sys.sched, sys.cpu.cycles);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "runner: no se pudo crear %s\n", path);
        return -1;
    }
    gc_gx_estado(&sys, estado_line, f);
    return (ferror(f) | fclose(f)) != 0 ? -1 : 0;
}

/* --vertices, --triangulos, --copias */
static FILE *vertices_file, *triangles_file, *copies_file;

static void triangle_line(void *ctx, const struct gx_rvtx *const v[3], uint32_t ntex)
{
    char text[GC_GX_TRIANGLE_TEXT];
    (void)ctx;
    gc_gx_triangle_text(v, ntex, text);
    fprintf(triangles_file, "%s\n", text);
}

static void copy_line(void *ctx, const char *text)
{
    (void)ctx;
    fprintf(copies_file, "%s\n", text);
}

/* Abre uno de los volcados de texto con su cabecera; -1 si no se pudo */
static int open_dump(FILE **f, const char *path, const char *header, int version)
{
    *f = fopen(path, "w");
    if (!*f) {
        fprintf(stderr, "%s: no se pudo crear\n", path);
        return -1;
    }
    setvbuf(*f, NULL, _IOFBF, 1u << 20);
    fprintf(*f, "# gc-ps3 %s v%d\n", header, version);
    return 0;
}

static int close_dump(FILE *f, const char *path)
{
    if (!f)
        return 0;
    if ((ferror(f) | fclose(f)) != 0) {
        fprintf(stderr, "runner: no se pudo escribir %s\n", path);
        return 1;
    }
    return 0;
}

static void vertex_line(void *ctx, const struct gx_vtx_out *v, uint32_t ntex)
{
    char text[GC_GX_VERTEX_TEXT];
    (void)ctx;
    gc_gx_vertex_text(v, ntex, text);
    fprintf(vertices_file, "%s\n", text);
}

static void report_line(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
    fflush(stdout);
}

/* programa.dol -> programa.map */
static void default_map(const char *dol, char *out, size_t cap)
{
    snprintf(out, cap, "%s", dol);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash) && (size_t)(dot - out) + 5 <= cap)
        strcpy(dot, ".map");
    else
        out[0] = '\0';
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end;
    if (!s || !*s)
        return -1;
    const unsigned long long v = strtoull(s, &end, 0);
    if (*end)
        return -1;
    *out = v;
    return 0;
}

/* --cuadros: el mismo formato que la app de PS3 (con "app:" en vez de "runner:") */
/* µs de procesador de la PC para el tiempo de GX (clock(), como el total) */
static uint64_t host_clock_us(void)
{
    return (uint64_t)((double)clock() * 1e6 / CLOCKS_PER_SEC);
}

static gc_frames_t frames;
static uint8_t frame_rgb[720 * 576 * 3];

static void print_frame(void *ctx, const gc_frame_t *f)
{
    (void)ctx;
    fprintf(stderr, "runner: cuadro %u (campo %llu) %ux%u, crc32 RGB 0x%08x\n", (unsigned)f->number,
            (unsigned long long)f->field, (unsigned)f->width, (unsigned)f->height, (unsigned)f->crc);
}

static int write_png(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
    int rc = 0;
    const size_t cap = gc_png_size(w, h);
    uint8_t *png = cap ? malloc(cap) : NULL;
    const size_t n = png ? gc_png_encode(rgb, w, h, png, cap) : 0;
    FILE *f = n ? fopen(path, "wb") : NULL;
    if (!f || fwrite(png, 1, n, f) != n) {
        fprintf(stderr, "runner: no se pudo escribir %s\n", path);
        rc = -1;
    } else {
        fprintf(stderr, "runner: imagen en %s\n", path);
    }
    if (f && fclose(f) != 0)
        rc = -1;
    free(png);
    return rc;
}

/* El color del EFB al terminar, en PNG */
static int write_efb(const char *path)
{
    gc_gx_efb_sync(&sys.gx);
    const uint32_t pf = sys.gx.bp[0x43] & 7u;
    uint8_t *rgb = malloc((size_t)GX_EFB_WIDTH * GX_EFB_HEIGHT * 3);
    if (!rgb)
        return -1;
    for (uint32_t y = 0; y < GX_EFB_HEIGHT; y++)
        for (uint32_t x = 0; x < GX_EFB_WIDTH; x++) {
            const uint32_t c = gx_efb_get_color(sys.gx.efb, pf, x, y);
            uint8_t *p = rgb + 3 * ((size_t)y * GX_EFB_WIDTH + x);
            p[0] = (uint8_t)(c >> 24);
            p[1] = (uint8_t)(c >> 16);
            p[2] = (uint8_t)(c >> 8);
        }
    const int rc = write_png(path, rgb, GX_EFB_WIDTH, GX_EFB_HEIGHT);
    free(rgb);
    return rc;
}

/* El último cuadro: tamaño y CRC por la salida de error; en PNG si hay archivo.
 * -1 si se pidió el PNG y no se pudo escribir. */
static int report_frame(const char *path)
{
    uint32_t w, h;
    const size_t size = gc_xfb_frame_size(&sys.vi, &w, &h);
    uint8_t *rgb = size ? malloc(size) : NULL;
    if (!rgb || gc_xfb_frame(&sys.mem, &sys.vi, rgb, size) != 0) {
        free(rgb);
        if (path)
            fprintf(stderr, "runner: sin cuadro de video, no se escribe %s\n", path);
        return path ? -1 : 0;
    }
    fprintf(stderr, "runner: cuadro %ux%u, crc32 RGB 0x%08x\n", (unsigned)w, (unsigned)h,
            (unsigned)gc_crc32(0, rgb, size));
    const int rc = path ? write_png(path, rgb, w, h) : 0;
    free(rgb);
    return rc;
}

int main(int argc, char **argv)
{
    uint64_t cycles = DEFAULT_CYCLES;
    const char *dol_path = NULL, *trace_path = NULL, *map_path = NULL, *image_path = NULL;
    const char *huella_path = NULL, *puntos_path = NULL, *detalle = NULL, *estado_path = NULL;
    const char *vertices_path = NULL, *triangles_path = NULL, *copies_path = NULL, *efb_path = NULL;
    gc_boot_config_t cfg = { 0, 0 };
    int have_rtc = 0, list_frames = 0, gx_lotes = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--version") == 0) {
            printf("%s %s (runner de PC)\n", GCPS3_NAME, GCPS3_VERSION);
            return 0;
        } else if (strcmp(a, "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        } else if (strcmp(a, "--ciclos") == 0) {
            if (parse_u64(++i < argc ? argv[i] : NULL, &cycles) != 0) {
                fprintf(stderr, "--ciclos necesita un numero\n");
                return 2;
            }
        } else if (strcmp(a, "--map") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--map necesita un archivo\n");
                return 2;
            }
            map_path = argv[i];
        } else if (strcmp(a, "--traza") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--traza necesita un archivo\n");
                return 2;
            }
            trace_path = argv[i];
        } else if (strcmp(a, "--imagen") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--imagen necesita un archivo\n");
                return 2;
            }
            image_path = argv[i];
        } else if (strcmp(a, "--efb") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--efb necesita un archivo\n");
                return 2;
            }
            efb_path = argv[i];
        } else if (strcmp(a, "--cuadros") == 0) {
            list_frames = 1;
        } else if (strcmp(a, "--gx-lotes") == 0) {
            gx_lotes = 1;
        } else if (strcmp(a, "--huella") == 0 || strcmp(a, "--puntos") == 0 ||
                   strcmp(a, "--detalle") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s necesita un argumento\n", a);
                return 2;
            }
            if (a[2] == 'h')
                huella_path = argv[i];
            else if (a[2] == 'p')
                puntos_path = argv[i];
            else
                detalle = argv[i];
        } else if (strcmp(a, "--estado-gx") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--estado-gx necesita un archivo\n");
                return 2;
            }
            estado_path = argv[i];
        } else if (strcmp(a, "--vertices") == 0 || strcmp(a, "--triangulos") == 0 ||
                   strcmp(a, "--copias") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s necesita un archivo\n", a);
                return 2;
            }
            if (a[2] == 'v')
                vertices_path = argv[i];
            else if (a[2] == 't')
                triangles_path = argv[i];
            else
                copies_path = argv[i];
        } else if (strcmp(a, "--pal") == 0) {
            cfg.pal = 1;
        } else if (strcmp(a, "--rtc") == 0) {
            if (parse_u64(++i < argc ? argv[i] : NULL, &cfg.rtc) != 0) {
                fprintf(stderr, "--rtc necesita un numero de segundos\n");
                return 2;
            }
            have_rtc = 1;
        } else if (a[0] == '-') {
            fprintf(stderr, "opcion desconocida: %s\n", a);
            usage(stderr, argv[0]);
            return 2;
        } else if (!dol_path) {
            dol_path = a;
        } else {
            fprintf(stderr, "sobra un argumento: %s\n", a);
            return 2;
        }
    }
    if (!dol_path) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (detalle) {
        char *colon;
        hout.desde = strtoull(detalle, &colon, 0);
        if (!huella_path || *colon != ':' || parse_u64(colon + 1, &hout.hasta) != 0 ||
            hout.hasta <= hout.desde) {
            fprintf(stderr, "--detalle necesita --huella y DESDE:HASTA con DESDE < HASTA\n");
            return 2;
        }
    }
    if (!have_rtc) {
        const time_t now = time(NULL);
        cfg.rtc = (uint64_t)now > GC_EPOCH_UNIX ? (uint64_t)now - GC_EPOCH_UNIX : 0;
    }

    size_t len = 0;
    uint8_t *data = read_file(dol_path, &len);
    if (!data) {
        fprintf(stderr, "%s: no se pudo leer (o pasa de 32 MB)\n", dol_path);
        return 1;
    }

    if (gc_system_init(&sys) != 0) {
        fprintf(stderr, "sin memoria para la GameCube emulada\n");
        free(data);
        return 1;
    }
    sys.gx.clock_us = host_clock_us;
    if (gx_lotes && gc_gx_set_backend(&sys.gx, &gx_backend_sw_lotes) != 0) {
        fprintf(stderr, "sin memoria para los lotes de GX\n");
        free(data);
        gc_system_free(&sys);
        return 1;
    }
    const gc_dol_error_t err = gc_system_boot_dol(&sys, data, len, &cfg);
    free(data);
    if (err != GC_DOL_OK) {
        fprintf(stderr, "%s: %s\n", dol_path, gc_dol_strerror(err));
        gc_system_free(&sys);
        return 1;
    }

    char guess[1024];
    if (!map_path) {
        default_map(dol_path, guess, sizeof guess);
        FILE *probe = guess[0] ? fopen(guess, "rb") : NULL;
        if (probe) {
            fclose(probe);
            map_path = guess;
        }
    }
    if (map_path) {
        size_t map_len = 0;
        uint8_t *map = read_file(map_path, &map_len);
        if (!map) {
            fprintf(stderr, "%s: no se pudo leer\n", map_path);
            gc_system_free(&sys);
            return 1;
        }
        gc_system_load_map(&sys, (const char *)map, map_len);
        free(map);
    }
    sys.hle.report = report_line;
    if (list_frames)
        gc_frames_attach(&frames, &sys, frame_rgb, sizeof frame_rgb, print_frame, NULL);

    if (trace_path) {
        trace_file = fopen(trace_path, "w");
        if (!trace_file) {
            fprintf(stderr, "%s: no se pudo crear\n", trace_path);
            gc_system_free(&sys);
            return 1;
        }
        setvbuf(trace_file, NULL, _IOFBF, 1u << 20);
        sys.cpu.trace = trace_line;
    }
    if (huella_path || puntos_path) {
        char det[1100];
        snprintf(det, sizeof det, "%s.detalle", huella_path ? huella_path : "");
        hout.huella = huella_path ? fopen(huella_path, "wb") : NULL;
        hout.puntos = puntos_path ? fopen(puntos_path, "w") : NULL;
        hout.detalle = detalle ? fopen(det, "wb") : NULL;
        if ((huella_path && !hout.huella) || (puntos_path && !hout.puntos) ||
            (detalle && !hout.detalle)) {
            fprintf(stderr, "no se pudo crear la huella, los puntos o el detalle\n");
            gc_system_free(&sys);
            return 1;
        }
        if (hout.huella)
            setvbuf(hout.huella, NULL, _IOFBF, 1u << 20);
        if (hout.puntos)
            fprintf(hout.puntos, "# gc-ps3 huella v%d runner, reloj %llu\n", GEKKO_HUELLA_VERSION,
                    (unsigned long long)cfg.rtc);
        sys.cpu.trace = trace_line;
    }

    if ((vertices_path && open_dump(&vertices_file, vertices_path, "vertices", GC_GX_VERTICES_VERSION)) ||
        (triangles_path && open_dump(&triangles_file, triangles_path, "triangulos", GC_GX_TRIANGULOS_VERSION)) ||
        (copies_path && open_dump(&copies_file, copies_path, "copias", GC_GX_COPIAS_VERSION))) {
        gc_system_free(&sys);
        return 1;
    }
    if (vertices_file)
        sys.gx.vertex_cb = vertex_line;
    if (copies_file)
        sys.gx.copy_cb = copy_line;
    if (triangles_file)
        sys.gx.triangle_cb = triangle_line;

    const clock_t t0 = clock();
    const uint64_t ran = gc_system_run(&sys, cycles);
    const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

    gc_hle_flush(&sys);
    if (trace_file)
        fclose(trace_file);
    if (hout.puntos && hout.n % PUNTOS_CADA != 0)
        fprintf(hout.puntos, "%llu %08x %016llx\n", (unsigned long long)hout.n,
                (unsigned)hout.last_pc, (unsigned long long)hout.cadena);
    int huella_failed = 0;
    FILE *outs[3] = { hout.huella, hout.detalle, hout.puntos };
    for (int k = 0; k < 3; k++)
        if (outs[k] && (ferror(outs[k]) | fclose(outs[k])) != 0)
            huella_failed = 1;
    if (huella_failed)
        fprintf(stderr, "runner: no se pudo escribir la huella, los puntos o el detalle\n");
    if (hout.huella || hout.puntos)
        fprintf(stderr, "runner: huella de %llu instrucciones, cadena %016llx\n",
                (unsigned long long)hout.n, (unsigned long long)hout.cadena);
    fprintf(stderr, "runner: %llu ciclos en %.2f s (%.1f MIPS), pc = 0x%08x, %u instrucciones ilegales\n",
            (unsigned long long)ran, secs, secs > 0 ? (double)ran / secs / 1e6 : 0.0,
            (unsigned)sys.cpu.pc, (unsigned)sys.cpu.illegal);
    if (sys.gx.xfb_copies)
        fprintf(stderr, "runner: GX %.1f ms, %u copias al XFB: %.2f ms por cuadro dibujado\n",
                (double)sys.gx.busy_us / 1000.0, (unsigned)sys.gx.xfb_copies,
                (double)sys.gx.busy_us / 1000.0 / (double)sys.gx.xfb_copies);
    else
        fprintf(stderr, "runner: GX %.1f ms\n", (double)sys.gx.busy_us / 1000.0);
    gx_lotes_stats_t lotes;
    if (gx_lotes_stats(&sys.gx, &lotes) == 0)
        fprintf(stderr, "runner: GX por lotes: %llu lotes, %llu triangulos (%.1f por lote, el mayor %u)\n",
                (unsigned long long)lotes.batches, (unsigned long long)lotes.triangles,
                lotes.batches ? (double)lotes.triangles / (double)lotes.batches : 0.0, (unsigned)lotes.max);
    if (sys.vi.last.xfb)
        fprintf(stderr, "runner: %llu campos de video, el ultimo de %ux%u con el XFB en 0x%08x\n",
                (unsigned long long)sys.vi.fields, (unsigned)sys.vi.last.width,
                (unsigned)sys.vi.last.height, (unsigned)sys.vi.last.xfb);
    else
        fprintf(stderr, "runner: %llu campos de video, ninguno con XFB\n",
                (unsigned long long)sys.vi.fields);
    const int image_failed = report_frame(image_path) != 0 || (efb_path && write_efb(efb_path) != 0);
    const int estado_failed = estado_path && write_estado_gx(estado_path) != 0;
    if (vertices_file)
        fprintf(stderr, "runner: %llu primitivas, %llu vertices transformados\n",
                (unsigned long long)sys.gx.primitives, (unsigned long long)sys.gx.drawn);
    if (copies_file)
        gc_gx_efb_final(&sys.gx);
    const int vertices_failed = close_dump(vertices_file, vertices_path) |
                                close_dump(triangles_file, triangles_path) |
                                close_dump(copies_file, copies_path);
    const int code = sys.hle.exited ? (int)(sys.hle.exit_code & 0xFFu) : 0;
    if (sys.hle.exited)
        fprintf(stderr, "runner: el programa termino con exit(%d)\n", (int)sys.hle.exit_code);
    else
        fprintf(stderr, "runner: se acabaron los ciclos sin exit\n");
    gc_system_free(&sys);
    return image_failed || huella_failed || estado_failed || vertices_failed ? 1 : code;
}
