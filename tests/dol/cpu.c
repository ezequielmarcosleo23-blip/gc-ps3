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
 * DOL de prueba "cpu": código C común compilado por un compilador real
 * (enteros, 64 bits con libgcc, punteros, recursión, switch, punto flotante)
 * más las particularidades del Gekko en ensamblador (paired singles, psq con
 * GQR, fres/frsqrte, el empate de fmadds).
 *
 * La parte común imprime resultados que se comparan con el mismo programa
 * compilado para la PC; la del Gekko compara contra valores conocidos e
 * imprime OK (en la PC, el OK se imprime directo).
 */
#include "rt.h"

static uint32_t lcg_state = 12345u;
static uint32_t lcg(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state >> 1;
}

/* ---- enteros ------------------------------------------------------------------- */

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void sort(int32_t *a, int n)
{
    if (n < 2)
        return;
    const int32_t pivot = a[n / 2];
    int i = 0, j = n - 1;
    while (i <= j) {
        while (a[i] < pivot)
            i++;
        while (a[j] > pivot)
            j--;
        if (i <= j) {
            const int32_t t = a[i];
            a[i++] = a[j];
            a[j--] = t;
        }
    }
    sort(a, j + 1);
    sort(a + i, n - i);
}

static int32_t fib(int32_t n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static uint32_t rotl(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static int32_t state_machine(const char *s)
{
    int32_t acc = 0;
    for (; *s; s++) {
        switch (*s) {
        case 'a': acc += 1; break;
        case 'b': acc *= 3; break;
        case 'c': acc -= 7; break;
        case 'd': acc ^= 0x55; break;
        case 'e': acc = (int32_t)((uint32_t)acc << 2); break;
        case 'f': acc >>= 1; break;
        case 'g': acc = -acc; break;
        case 'h': acc += 1000; break;
        case 'i': acc %= 97; break;
        default:  acc += *s; break;
        }
    }
    return acc;
}

static uint8_t buf[4096];
static int32_t arr[512];

static void integers(void)
{
    for (unsigned i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)lcg();
    OSReport("crc32: %08x\n", (unsigned)crc32(buf, sizeof buf));

    for (int i = 0; i < 512; i++)
        arr[i] = (int32_t)(lcg() % 20001u) - 10000;
    sort(arr, 512);
    int ok = 1;
    int64_t sum = 0;
    for (int i = 0; i < 512; i++) {
        if (i && arr[i - 1] > arr[i])
            ok = 0;
        sum += (int64_t)arr[i] * (i + 1);
    }
    OSReport("orden: %s, min %d, max %d, suma ponderada %lld\n", ok ? "si" : "no", (int)arr[0],
             (int)arr[511], (long long)sum);

    OSReport("fib(20): %d\n", (int)fib(20));
    OSReport("rotaciones: %08x %08x clz %d\n", (unsigned)rotl(0x12345678u, 8), (unsigned)rotl(0x80000001u, 31),
             __builtin_clz(0x00012345u));
    OSReport("switch: %d\n", (int)state_machine("abacabadhefgihxyz"));

    int64_t a = 123456789012345ll, b = -9876543ll;
    uint64_t u = 0xFEDCBA9876543210ull;
    OSReport("64 bits: %lld %lld %lld %llu %llx %llx\n", (long long)(a * b), (long long)(a / b),
             (long long)(a % b), (unsigned long long)(u / 1000003u), (unsigned long long)(u >> 13),
             (unsigned long long)(u << 7));

    int32_t m = -1073741824, n = 3;
    uint32_t um = 0xFFFFFFF0u;
    OSReport("32 bits: %d %d %u %u %d %d\n", (int)(m / n), (int)(m % n), (unsigned)(um / 7u),
             (unsigned)(um % 7u), (int)((int64_t)m * n >> 32), (int)(m >> 3));
}

/* ---- punto flotante -------------------------------------------------------------- */

static double newton_sqrt(double x)
{
    double r = x > 1.0 ? x / 2.0 : 1.0;
    for (int i = 0; i < 60; i++)
        r = 0.5 * (r + x / r);
    return r;
}

static float mat[4][4], acc[4][4];

static void floats(void)
{
    double h = 0.0;
    for (int k = 1; k <= 1000; k++)
        h += 1.0 / k;
    float hf = 0.0f;
    for (int k = 1; k <= 1000; k++)
        hf += 1.0f / (float)k;
    OSReport("armonica: %.17g %.9g\n", h, (double)hf);
    OSReport("raices: %.17g %.17g\n", newton_sqrt(2.0), newton_sqrt(1e10));

    int inside = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 32; x++) {
            const double cr = -2.0 + x * (2.5 / 32), ci = -1.0 + y * (2.0 / 16);
            double zr = 0, zi = 0;
            int it = 0;
            while (it < 50 && zr * zr + zi * zi < 4.0) {
                const double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
                it++;
            }
            inside += it;
        }
    OSReport("mandelbrot: %d\n", inside);

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            mat[i][j] = (float)(i * 4 + j + 1) / 16.0f;
            acc[i][j] = i == j ? 1.0f : 0.0f;
        }
    for (int r = 0; r < 8; r++) {
        float t[4][4];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                float s = 0.0f;
                for (int k = 0; k < 4; k++)
                    s += acc[i][k] * mat[k][j];
                t[i][j] = s;
            }
        memcpy(acc, t, sizeof acc);
    }
    OSReport("matriz: %.9g %.9g %.9g %.9g\n", (double)acc[0][0], (double)acc[1][2], (double)acc[2][1],
             (double)acc[3][3]);

    volatile double dv = -123.75, big = 3e9;
    volatile float fv = 2.5e9f;
    volatile int32_t iv = -7;
    volatile uint32_t uv = 3000000000u;
    OSReport("conversiones: %d %u %u %d %.1f %.1f %.1f\n", (int)dv, (unsigned)big, (unsigned)fv,
             (int)(float)iv, (double)iv, (double)uv, (double)(float)uv);
}

/* ---- Gekko ----------------------------------------------------------------------- */

#ifdef GCPS3_HOST
static void gekko(void)
{
    OSReport("gekko: ps OK, psq OK, fres OK, frsqrte OK, fmadds OK, tb OK, sc OK\n");
}
#else
static uint64_t dbits(double d)
{
    union { double d; uint64_t u; } x = { d };
    return x.u;
}

static double ps_check(int *ok)
{
    /* f1 = {1.5, -2.0}, f2 = {0.25, 8.0} */
    static const float in[4] = { 1.5f, -2.0f, 0.25f, 8.0f };
    double p0, p1, s0, s1;
    __asm__ volatile("psq_l 1, 0(%4), 0, 0\n\t"
                     "psq_l 2, 8(%4), 0, 0\n\t"
                     "ps_add 3, 1, 2\n\t"           /* {1.75, 6.0} */
                     "ps_mul 4, 1, 2\n\t"           /* {0.375, -16.0} */
                     "ps_merge10 5, 3, 4\n\t"       /* {6.0, 0.375} */
                     "ps_sum0 6, 5, 4, 3\n\t"       /* ps0 = f5.ps0 + f3.ps1 = 12, ps1 = f4.ps1 = -16 */
                     "fmr %0, 5\n\t"
                     "ps_merge11 7, 5, 5\n\t"
                     "fmr %1, 7\n\t"
                     "fmr %2, 6\n\t"
                     "ps_merge11 7, 6, 6\n\t"
                     "fmr %3, 7\n\t"
                     : "=f"(p0), "=f"(p1), "=f"(s0), "=f"(s1)
                     : "b"(in)
                     : "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "memory");
    *ok = p0 == 6.0 && p1 == 0.375 && s0 == 12.0 && s1 == -16.0;
    return p0;
}

/* GQR2: guarda como u8 x 16 (escala 4), carga como s8 / 2 (escala 1) */
static int psq_check(void)
{
    static const float in[2] = { 1.5f, 20.0f };      /* x16 = 24 y 320 -> 255 */
    static uint8_t outb[4];
    double l0, l1;
    const uint32_t gqr = (1u << 24) | (6u << 16) | (4u << 8) | 4u;
    __asm__ volatile("mtspr 914, %3\n\t"
                     "psq_l 1, 0(%4), 0, 0\n\t"
                     "psq_st 1, 0(%5), 0, 2\n\t"
                     "psq_l 2, 0(%5), 0, 2\n\t"
                     "fmr %0, 2\n\t"
                     "ps_merge11 2, 2, 2\n\t"
                     "fmr %1, 2\n\t"
                     : "=f"(l0), "=f"(l1), "=m"(outb)
                     : "r"(gqr), "b"(in), "b"(outb)
                     : "fr1", "fr2", "memory");
    /* 24 = 0x18 -> s8 24 / 2 = 12; 255 = 0xFF -> s8 -1 / 2 = -0.5 */
    return outb[0] == 24 && outb[1] == 255 && l0 == 12.0 && l1 == -0.5;
}

static int estimates_check(void)
{
    double one = 1.0, r, q;
    __asm__ volatile("fres %0, %2\n\t"
                     "frsqrte %1, %2"
                     : "=&f"(r), "=&f"(q)
                     : "f"(one));
    return dbits(r) == 0x3FEFFF0000000000ull && dbits(q) == 0x3FEFFE8000000000ull;
}

/* El empate de Mario Strikers Charged (Dolphin): un FMA doble redondeado
 * después a simple da 0xbf55bf18; el Gekko, 0xbf55bf17. */
static int fmadds_check(void)
{
    union { uint32_t u; float f; } a = { 0x42480000u }, c = { 0xBC88CC38u }, b = { 0x1B1C72A0u }, r;
    __asm__ volatile("fmadds %0, %1, %2, %3" : "=f"(r.f) : "f"(a.f), "f"(c.f), "f"(b.f));
    return r.u == 0xBF55BF17u;
}

static int tb_check(void)
{
    uint32_t lo1, lo2, up;
    __asm__ volatile("mftb %0\n\tmftbu %1" : "=r"(lo1), "=r"(up));
    for (volatile int i = 0; i < 100; i++)
        ;
    __asm__ volatile("mftb %0" : "=r"(lo2));
    (void)up;
    return lo2 > lo1;
}

/* sc con el rfi que deja el IPL en 0xC00: vuelve a la instrucción siguiente */
static int sc_check(void)
{
    int r = 0;
    __asm__ volatile("li %0, 1\n\tsc\n\taddi %0, %0, 1" : "=&r"(r));
    return r == 2;
}

static void gekko(void)
{
    int ps_ok;
    ps_check(&ps_ok);
    OSReport("gekko: ps %s, psq %s, fres %s, frsqrte %s, fmadds %s, tb %s, sc %s\n", ps_ok ? "OK" : "FALLO",
             psq_check() ? "OK" : "FALLO", estimates_check() ? "OK" : "FALLO",
             estimates_check() ? "OK" : "FALLO", fmadds_check() ? "OK" : "FALLO", tb_check() ? "OK" : "FALLO",
             sc_check() ? "OK" : "FALLO");
}
#endif

int main(void)
{
    OSReport("cpu: inicio\n");
    integers();
    floats();
    gekko();
    OSReport("cpu: fin\n");
    return 0;
}
