/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cola NOBITS al final del segmento de texto (hdr_text de lv2.ld) para la
 * prueba "jit". Al ir detrás de todo lo que ocupa archivo, el segmento queda
 * con memsz > filesz: el cargador la rellena con ceros, como .bss, sin que
 * ocupe espacio en el .self. Con HEN es el único lugar escribible y ejecutable
 * (lo midió Salsa64 en hardware real: tests/phase0/jit-ppe/nobits.ld de ese
 * proyecto, GPLv2+).
 *
 * Se agrega a lv2.ld con INSERT: se pasa -T nobits.ld -T lv2.ld, en ese orden
 * (con -T, gcc ya no agrega lv2.ld solo). La sección la define jit/main.c.
 */
SECTIONS
{
	.gcps3_jit_nobits (NOLOAD) : ALIGN(64K) { KEEP (*(.gcps3_jit_nobits)) } : hdr_text
}
INSERT AFTER .sys_proc_prx_param;
