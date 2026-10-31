!!ARBfp1.0
OPTION NV_fragment_program2;
# gc-ps3: condiciones y KIL (test_rsxfp.c arma lo mismo)
#const c[0] = 0.5 1 2 3
PARAM c[1] = { { 0.5, 1, 2, 3 } };
TEMP R0;
TEMP R1;
TEMP R2;
TEMP RC;
OUTPUT oCol = result.color;
MOVR R1, fragment.texcoord[3];
SLTRC RC.x, R1.xxxx, c[0].yyyy;
SGERC RC.yz, R1, c[0].xxxx;
MOVR R2, c[0];
MOVR R2(GT.x), R1;
MOVR R2(LT.yzwx), -R1;
MOVRC R0, R2;
KIL LE.y;
KIL NE.wwww;
END
