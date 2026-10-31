!!ARBfp1.0
OPTION NV_fragment_program2;
# gc-ps3: texturas y Z (test_rsxfp.c arma lo mismo)
TEMP R0;
TEMP R1;
TEMP R2;
OUTPUT oCol = result.color;
TEX R2, fragment.texcoord[2], texture[3], 2D;
MULR R1, fragment.texcoord[5], R2;
TEX R1, R1, texture[15], 2D;
TXP R0, fragment.texcoord[7], texture[0], 2D;
ADDR R0, R0, R1;
MOVR result.depth.z, R2.wwww;
END
