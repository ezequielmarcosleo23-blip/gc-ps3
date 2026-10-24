#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Genera las constantes de src/gx/exp2.c: 2^(j/64) para j = 0..63 como
# double-double (hi + lo), ln 2 partido en tres (dos de 28 bits y el resto) y
# los coeficientes 1/n! del polinomio. Todo con el módulo decimal a 60
# dígitos; float(Decimal) redondea al double más cercano.
#
#   python3 tools/exp2/gen_tabla.py > /tmp/tabla.c

from decimal import Decimal, getcontext
from math import frexp

getcontext().prec = 60


def hexd(v):
    return float(v).hex()


def dd(v):
    hi = float(v)
    lo = float(v - Decimal(hi))
    return hi, lo


def corta(v, bits):
    # v redondeado a 'bits' bits significativos
    e = frexp(float(v))[1]
    n = (v / Decimal(2) ** (e - bits)).to_integral_value()
    r = Decimal(n) * Decimal(2) ** (e - bits)
    rf = float(r)
    assert Decimal(rf) == r
    return rf


ln2 = Decimal(2).ln()
l1 = corta(ln2, 28)
l2 = corta(ln2 - Decimal(l1), 28)
l3 = float(ln2 - Decimal(l1) - Decimal(l2))

print("/* ln 2 = L1 + L2 + L3; L1 y L2 tienen 28 bits significativos */")
print("#define L1 %s" % hexd(l1))
print("#define L2 %s" % hexd(l2))
print("#define L3 %s" % hexd(l3))
print()

fact = Decimal(1)
coef = []
for n in range(11):
    if n > 0:
        fact *= n
    coef.append(Decimal(1) / fact)
print("/* 1/n! */")
for n in (3, 4):
    hi, lo = dd(coef[n])
    print("#define C%d_HI %s" % (n, hexd(hi)))
    print("#define C%d_LO %s" % (n, hexd(lo)))
for n in range(5, 11):
    print("#define C%d %s" % (n, hexd(float(coef[n]))))
print()

print("/* 2^(j/64): hi, lo */")
print("static const double tabla[64][2] = {")
for j in range(64):
    v = Decimal(2) ** (Decimal(j) / Decimal(64))
    hi, lo = dd(v)
    print("    { %s, %s }," % (hexd(hi), hexd(lo)))
print("};")
