!!ARBfp1.0
OPTION NV_fragment_program2;
# gc-ps3: aritmética de fragment programs (test_rsxfp.c arma lo mismo)
#const c[0] = 0.00390625 0.5 255 -3
PARAM c[1] = { { 0.00390625, 0.5, 255, -3 } };
TEMP R0;
TEMP R1;
TEMP R2;
TEMP R3;
TEMP R4;
OUTPUT oCol = result.color;
MOVR R1, |fragment.texcoord[1]|;
MULR R2, fragment.texcoord[0], c[0].zzzz;
ADDR R3, |R1|, -|R2.wzyx|;
MADR R4, R1, |R2|, -R3;
FLRR R2, R2;
FRCR R3.xy, R4;
DP3R R4.x, R1, R2;
DP4R R4.y, R1, R2;
MINR R4.z, R1.xxxx, c[0].wwww;
MAXR R4.w, R1.yyyy, -c[0].yyyy;
SLTR R1, R2, R3;
SGER R1.xy, R2, R3;
SLER R1.zw, R2, R3;
SGTR R2, R3, R4;
SNER R2.xz, R3, R4;
SEQR R2.yw, R3, R4;
RCPR R3.z, R4.wwww;
EX2R R3.w, R2.yyyy;
LG2R R3.x, R4.zzzz;
MOVR R0, fragment.color.primary;
ADDR R0, fragment.color.secondary, R0;
MADR R0, fragment.fogcoord, R1, R0;
MULR R0, fragment.position, R0;
MADR_SAT R0.xw, R3, c[0].xxxx, R4;
END
