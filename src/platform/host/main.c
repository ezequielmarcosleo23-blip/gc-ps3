/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Runner en PC: arranca un DOL por HLE y lo corre en el intérprete de Gekko,
 * sin consola ni pantalla (la imagen llega en la etapa E de la Fase 1).
 *
 *   gc-ps3-runner [opciones] programa.dol
 *     --ciclos N       cuántos ciclos de CPU correr (por omisión 486 000 000 = 1 s)
 *     --traza ARCHIVO  una línea por instrucción: "pc instrucción" en hexadecimal
 *     --pal            el IPL dejó modo PAL (por omisión NTSC)
 *     --rtc SEGUNDOS   reloj al arrancar, en segundos desde el 2000-01-01
 *                      (por omisión, la hora de la PC)
 *
 * Sale con 0 si el DOL corrió, 1 si no se pudo cargar y 2 si las opciones
 * están mal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/log.h"
#include "core/system.h"
#include "core/version.h"

#define DEFAULT_CYCLES GC_CPU_CLOCK
#define MAX_FILE       (32u << 20)

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "uso: %s [opciones] programa.dol\n"
            "  --ciclos N       ciclos de CPU a correr (por omision %u, 1 s)\n"
            "  --traza ARCHIVO  pc e instruccion de cada paso\n"
            "  --pal            modo de video PAL (por omision NTSC)\n"
            "  --rtc SEGUNDOS   reloj al arrancar, segundos desde el 2000-01-01\n"
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

static void trace_line(void *ctx, uint32_t pc, uint32_t inst)
{
    fprintf((FILE *)ctx, "%08x %08x\n", (unsigned)pc, (unsigned)inst);
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

int main(int argc, char **argv)
{
    uint64_t cycles = DEFAULT_CYCLES;
    const char *dol_path = NULL, *trace_path = NULL;
    gc_boot_config_t cfg = { 0, 0 };
    int have_rtc = 0;

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
        } else if (strcmp(a, "--traza") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--traza necesita un archivo\n");
                return 2;
            }
            trace_path = argv[i];
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

    static gc_system_t sys;
    if (gc_system_init(&sys) != 0) {
        fprintf(stderr, "sin memoria para la GameCube emulada\n");
        free(data);
        return 1;
    }
    const gc_dol_error_t err = gc_system_boot_dol(&sys, data, len, &cfg);
    free(data);
    if (err != GC_DOL_OK) {
        fprintf(stderr, "%s: %s\n", dol_path, gc_dol_strerror(err));
        gc_system_free(&sys);
        return 1;
    }

    FILE *trace = NULL;
    if (trace_path) {
        trace = fopen(trace_path, "w");
        if (!trace) {
            fprintf(stderr, "%s: no se pudo crear\n", trace_path);
            gc_system_free(&sys);
            return 1;
        }
        setvbuf(trace, NULL, _IOFBF, 1u << 20);
        sys.cpu.trace = trace_line;
        sys.cpu.trace_ctx = trace;
    }

    const clock_t t0 = clock();
    const uint64_t ran = gc_system_run(&sys, cycles);
    const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

    if (trace)
        fclose(trace);
    fprintf(stderr, "runner: %llu ciclos en %.2f s (%.1f MIPS), pc = 0x%08x, %u instrucciones ilegales\n",
            (unsigned long long)ran, secs, secs > 0 ? (double)ran / secs / 1e6 : 0.0,
            (unsigned)sys.cpu.pc, (unsigned)sys.cpu.illegal);
    gc_system_free(&sys);
    return 0;
}
