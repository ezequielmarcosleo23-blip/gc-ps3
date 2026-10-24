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
 * 2^x = 2^k * 2^(j/64) * e^(r ln 2), con m = 64k + j el entero más cercano a
 * 64x y r = x - m/64 (|r| <= 1/128). 2^(j/64) sale de una tabla y e^s de un
 * polinomio de grado 10; los dos en double-double (un valor = hi + lo), con
 * unos 100 bits de precisión. Las constantes las genera
 * tools/exp2/gen_tabla.py.
 *
 * Las operaciones double-double necesitan cada suma y producto redondeado
 * por separado: el archivo se compila con -ffp-contract=off (sin fma).
 */
#include "gx/exp2.h"

typedef struct { double hi, lo; } dd_t;

/* ln 2 = L1 + L2 + L3; L1 y L2 tienen 28 bits significativos */
#define L1 0x1.62e42fe000000p-1
#define L2 0x1.f473de6000000p-30
#define L3 0x1.5e4f1d9cc01f9p-59

/* 1/n! */
#define C3_HI 0x1.5555555555555p-3
#define C3_LO 0x1.5555555555555p-57
#define C4_HI 0x1.5555555555555p-5
#define C4_LO 0x1.5555555555555p-59
#define C5 0x1.1111111111111p-7
#define C6 0x1.6c16c16c16c17p-10
#define C7 0x1.a01a01a01a01ap-13
#define C8 0x1.a01a01a01a01ap-16
#define C9 0x1.71de3a556c734p-19
#define C10 0x1.27e4fb7789f5cp-22

/* 2^(j/64): hi, lo */
static const double tabla[64][2] = {
    { 0x1.0000000000000p+0, 0x0.0p+0 },
    { 0x1.02c9a3e778061p+0, -0x1.19083535b085dp-56 },
    { 0x1.059b0d3158574p+0, 0x1.d73e2a475b465p-55 },
    { 0x1.0874518759bc8p+0, 0x1.186be4bb284ffp-57 },
    { 0x1.0b5586cf9890fp+0, 0x1.8a62e4adc610bp-54 },
    { 0x1.0e3ec32d3d1a2p+0, 0x1.03a1727c57b53p-59 },
    { 0x1.11301d0125b51p+0, -0x1.6c51039449b3ap-54 },
    { 0x1.1429aaea92de0p+0, -0x1.32fbf9af1369ep-54 },
    { 0x1.172b83c7d517bp+0, -0x1.19041b9d78a76p-55 },
    { 0x1.1a35beb6fcb75p+0, 0x1.e5b4c7b4968e4p-55 },
    { 0x1.1d4873168b9aap+0, 0x1.e016e00a2643cp-54 },
    { 0x1.2063b88628cd6p+0, 0x1.dc775814a8495p-55 },
    { 0x1.2387a6e756238p+0, 0x1.9b07eb6c70573p-54 },
    { 0x1.26b4565e27cddp+0, 0x1.2bd339940e9d9p-55 },
    { 0x1.29e9df51fdee1p+0, 0x1.612e8afad1255p-55 },
    { 0x1.2d285a6e4030bp+0, 0x1.0024754db41d5p-54 },
    { 0x1.306fe0a31b715p+0, 0x1.6f46ad23182e4p-55 },
    { 0x1.33c08b26416ffp+0, 0x1.32721843659a6p-54 },
    { 0x1.371a7373aa9cbp+0, -0x1.63aeabf42eae2p-54 },
    { 0x1.3a7db34e59ff7p+0, -0x1.5e436d661f5e3p-56 },
    { 0x1.3dea64c123422p+0, 0x1.ada0911f09ebcp-55 },
    { 0x1.4160a21f72e2ap+0, -0x1.ef3691c309278p-58 },
    { 0x1.44e086061892dp+0, 0x1.89b7a04ef80d0p-59 },
    { 0x1.486a2b5c13cd0p+0, 0x1.3c1a3b69062f0p-56 },
    { 0x1.4bfdad5362a27p+0, 0x1.d4397afec42e2p-56 },
    { 0x1.4f9b2769d2ca7p+0, -0x1.4b309d25957e3p-54 },
    { 0x1.5342b569d4f82p+0, -0x1.07abe1db13cadp-55 },
    { 0x1.56f4736b527dap+0, 0x1.9bb2c011d93adp-54 },
    { 0x1.5ab07dd485429p+0, 0x1.6324c054647adp-54 },
    { 0x1.5e76f15ad2148p+0, 0x1.ba6f93080e65ep-54 },
    { 0x1.6247eb03a5585p+0, -0x1.383c17e40b497p-54 },
    { 0x1.6623882552225p+0, -0x1.bb60987591c34p-54 },
    { 0x1.6a09e667f3bcdp+0, -0x1.bdd3413b26456p-54 },
    { 0x1.6dfb23c651a2fp+0, -0x1.bbe3a683c88abp-57 },
    { 0x1.71f75e8ec5f74p+0, -0x1.16e4786887a99p-55 },
    { 0x1.75feb564267c9p+0, -0x1.0245957316dd3p-54 },
    { 0x1.7a11473eb0187p+0, -0x1.41577ee04992fp-55 },
    { 0x1.7e2f336cf4e62p+0, 0x1.05d02ba15797ep-56 },
    { 0x1.82589994cce13p+0, -0x1.d4c1dd41532d8p-54 },
    { 0x1.868d99b4492edp+0, -0x1.fc6f89bd4f6bap-54 },
    { 0x1.8ace5422aa0dbp+0, 0x1.6e9f156864b27p-54 },
    { 0x1.8f1ae99157736p+0, 0x1.5cc13a2e3976cp-55 },
    { 0x1.93737b0cdc5e5p+0, -0x1.75fc781b57ebcp-57 },
    { 0x1.97d829fde4e50p+0, -0x1.d185b7c1b85d1p-54 },
    { 0x1.9c49182a3f090p+0, 0x1.c7c46b071f2bep-56 },
    { 0x1.a0c667b5de565p+0, -0x1.359495d1cd533p-54 },
    { 0x1.a5503b23e255dp+0, -0x1.d2f6edb8d41e1p-54 },
    { 0x1.a9e6b5579fdbfp+0, 0x1.0fac90ef7fd31p-54 },
    { 0x1.ae89f995ad3adp+0, 0x1.7a1cd345dcc81p-54 },
    { 0x1.b33a2b84f15fbp+0, -0x1.2805e3084d708p-57 },
    { 0x1.b7f76f2fb5e47p+0, -0x1.5584f7e54ac3bp-56 },
    { 0x1.bcc1e904bc1d2p+0, 0x1.23dd07a2d9e84p-55 },
    { 0x1.c199bdd85529cp+0, 0x1.11065895048ddp-55 },
    { 0x1.c67f12e57d14bp+0, 0x1.2884dff483cadp-54 },
    { 0x1.cb720dcef9069p+0, 0x1.503cbd1e949dbp-56 },
    { 0x1.d072d4a07897cp+0, -0x1.cbc3743797a9cp-54 },
    { 0x1.d5818dcfba487p+0, 0x1.2ed02d75b3707p-55 },
    { 0x1.da9e603db3285p+0, 0x1.c2300696db532p-54 },
    { 0x1.dfc97337b9b5fp+0, -0x1.1a5cd4f184b5cp-54 },
    { 0x1.e502ee78b3ff6p+0, 0x1.39e8980a9cc8fp-55 },
    { 0x1.ea4afa2a490dap+0, -0x1.e9c23179c2893p-54 },
    { 0x1.efa1bee615a27p+0, 0x1.dc7f486a4b6b0p-54 },
    { 0x1.f50765b6e4540p+0, 0x1.9d3e12dd8a18bp-54 },
    { 0x1.fa7c1819e90d8p+0, 0x1.74853f3a5931ep-55 },
};

/* a + b exacto = s + e */
static dd_t two_sum(double a, double b)
{
    const double s = a + b;
    const double bb = s - a;
    const dd_t r = { s, (a - (s - bb)) + (b - bb) };
    return r;
}

/* Lo mismo con |a| >= |b| */
static dd_t fast_two_sum(double a, double b)
{
    const double s = a + b;
    const dd_t r = { s, b - (s - a) };
    return r;
}

/* a = hi + lo con 26 bits cada uno (Veltkamp) */
static void split(double a, double *hi, double *lo)
{
    const double t = 134217729.0 * a; /* 2^27 + 1 */
    *hi = t - (t - a);
    *lo = a - *hi;
}

/* a * b exacto = p + e (Dekker) */
static dd_t two_prod(double a, double b)
{
    double ah, al, bh, bl;
    const double p = a * b;
    split(a, &ah, &al);
    split(b, &bh, &bl);
    const dd_t r = { p, ((ah * bh - p) + ah * bl + al * bh) + al * bl };
    return r;
}

static dd_t dd_mul(dd_t a, dd_t b)
{
    dd_t p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return fast_two_sum(p.hi, p.lo);
}

static dd_t dd_add(dd_t a, double bhi, double blo)
{
    dd_t s = two_sum(a.hi, bhi);
    s.lo += a.lo + blo;
    return fast_two_sum(s.hi, s.lo);
}

double gx_exp2(float xf)
{
    const double x = xf;
    if (x != x)
        return x;

    /* m = el entero más cercano a 64x (64x es exacto) */
    const double t = x * 64.0;
    int m = (int)t;
    const double d = t - (double)m;
    if (d > 0.5)
        m++;
    else if (d < -0.5)
        m--;
    const int j = m & 63;
    int k = (m - j) / 64;

    /* r = x - m/64 es exacto (Sterbenz) y tiene a lo sumo 24 bits
     * significativos, así que r * L1 y r * L2 también son exactos */
    const double r = x - (double)m / 64.0;
    dd_t s = fast_two_sum(r * L1, r * L2);
    s = fast_two_sum(s.hi, s.lo + r * L3);

    /* e^s: la cola en double (aporta menos de 2^-44) y el resto en
     * double-double, por Horner */
    const double c = C5 + s.hi * (C6 + s.hi * (C7 + s.hi * (C8 + s.hi * (C9 + s.hi * C10))));
    dd_t p = { c, 0.0 };
    p = dd_add(dd_mul(p, s), C4_HI, C4_LO);
    p = dd_add(dd_mul(p, s), C3_HI, C3_LO);
    p = dd_add(dd_mul(p, s), 0.5, 0.0);
    p = dd_add(dd_mul(p, s), 1.0, 0.0);
    p = dd_add(dd_mul(p, s), 1.0, 0.0);

    /* 2^(j/64) * e^s redondeado a double, y por 2^k (exacto) */
    const dd_t tj = { tabla[j][0], tabla[j][1] };
    double v = dd_mul(tj, p).hi;
    for (; k < 0; k++)
        v *= 0.5;
    for (; k > 0; k--)
        v *= 2.0;
    return v;
}
