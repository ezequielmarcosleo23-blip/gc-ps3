#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""ELF de PowerPC (32 bits, big-endian) a DOL de GameCube, con su .map.

    python3 tools/elf2dol.py programa.elf programa.dol [--map programa.map]
    python3 tools/elf2dol.py --selftest

Cada segmento PT_LOAD con datos va a una sección del DOL: los ejecutables a
las de texto (hasta 7) y el resto a las de datos (hasta 11). Lo que un
segmento tiene en memoria y no en el archivo (memsz > filesz) es el BSS.
Las secciones se rellenan hasta un múltiplo de 32 bytes, como las copia el
IPL, y quedan en el archivo alineadas a 32 desde la cabecera de 0x100.

El .map tiene el formato que escribe Dolphin con "Save Symbol Map" (las
funciones en ".text section layout" y los datos en ".data section layout"):
el emulador lo usa para los ganchos de depuración (src/core/hle.c) y Dolphin
lo puede cargar igual.
"""
import argparse
import os
import struct
import sys
import tempfile

sys.dont_write_bytecode = True

PT_LOAD = 1
PF_X = 1
SHT_SYMTAB = 2
STT_OBJECT, STT_FUNC = 1, 2
MAX_TEXT, MAX_DATA = 7, 11
HEADER = 0x100


class ElfError(Exception):
    pass


def parse_elf(data):
    """Segmentos PT_LOAD, entrada y símbolos de un ELF32 big-endian."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 2:
        raise ElfError("no es un ELF de 32 bits big-endian")
    (e_type, e_machine, _ver, entry, phoff, shoff, _flags, _ehsize, phentsize, phnum,
     shentsize, shnum, _shstrndx) = struct.unpack_from(">HHIIIIIHHHHHH", data, 16)
    if e_machine != 20:
        raise ElfError("no es PowerPC (e_machine %d)" % e_machine)
    segs = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_flags, _align = struct.unpack_from(
            ">IIIIIIII", data, phoff + i * phentsize)
        if p_type == PT_LOAD and p_memsz:
            segs.append({"offset": p_offset, "vaddr": p_vaddr, "filesz": p_filesz,
                         "memsz": p_memsz, "exec": bool(p_flags & PF_X)})
    syms = []
    sections = [struct.unpack_from(">IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    for sh in sections:
        if sh[1] != SHT_SYMTAB:
            continue
        strtab = sections[sh[6]]
        for k in range(sh[5] // 16):
            st_name, st_value, st_size, st_info, _other, _shndx = struct.unpack_from(
                ">IIIBBH", data, sh[4] + k * 16)
            kind = st_info & 0xF
            if kind not in (STT_FUNC, STT_OBJECT) or not st_size:
                continue
            start = strtab[4] + st_name
            name = data[start:data.index(b"\0", start)].decode("ascii", "replace")
            syms.append((st_value, st_size, kind, name))
    return entry, segs, sorted(syms)


def build_dol(entry, segs, data):
    texts = [s for s in segs if s["exec"] and s["filesz"]]
    datas = [s for s in segs if not s["exec"] and s["filesz"]]
    if not texts:
        raise ElfError("no hay segmentos ejecutables")
    if len(texts) > MAX_TEXT or len(datas) > MAX_DATA:
        raise ElfError("demasiados segmentos para un DOL")
    bss_lo = min((s["vaddr"] + s["filesz"] for s in segs if s["memsz"] > s["filesz"]), default=0)
    bss_hi = max((s["vaddr"] + s["memsz"] for s in segs if s["memsz"] > s["filesz"]), default=0)

    header = bytearray(HEADER)
    body = bytearray()
    for slot, seg in [(i, s) for i, s in enumerate(texts)] + [(7 + i, s) for i, s in enumerate(datas)]:
        chunk = bytes(data[seg["offset"]:seg["offset"] + seg["filesz"]])
        chunk += b"\0" * (-len(chunk) % 32)
        offset = HEADER + len(body)
        struct.pack_into(">I", header, 0x00 + 4 * slot, offset)
        struct.pack_into(">I", header, 0x48 + 4 * slot, seg["vaddr"])
        struct.pack_into(">I", header, 0x90 + 4 * slot, len(chunk))
        body += chunk
    struct.pack_into(">III", header, 0xD8, bss_lo, bss_hi - bss_lo, entry)
    return bytes(header + body)


def build_map(syms):
    lines = [".text section layout"]
    lines += ["%08x %06x %08x 0 %s" % (a, n, a, name) for a, n, k, name in syms if k == STT_FUNC]
    lines += ["", ".data section layout"]
    lines += ["%08x %06x %08x 0 %s" % (a, n, a, name) for a, n, k, name in syms if k == STT_OBJECT]
    return "\n".join(lines) + "\n"


def convert(elf_path, dol_path, map_path=None):
    with open(elf_path, "rb") as f:
        data = f.read()
    entry, segs, syms = parse_elf(data)
    with open(dol_path, "wb") as f:
        f.write(build_dol(entry, segs, data))
    if map_path:
        with open(map_path, "w", encoding="ascii") as f:
            f.write(build_map(syms))


def selftest():
    def check(cond, what):
        if not cond:
            raise SystemExit("elf2dol selftest: FALLO: " + what)

    # ELF mínimo a mano: un segmento de texto, uno de datos con BSS y un símbolo
    text = struct.pack(">II", 0x38600005, 0x4E800020)          # li r3,5; blr
    datab = b"\x11\x22\x33"
    strtab = b"\0main\0valor\0"
    syms = struct.pack(">IIIBBH", 0, 0, 0, 0, 0, 0)
    syms += struct.pack(">IIIBBH", 1, 0x80003100, 8, (1 << 4) | STT_FUNC, 0, 1)
    syms += struct.pack(">IIIBBH", 6, 0x80004000, 3, (1 << 4) | STT_OBJECT, 0, 2)
    ph_off, text_off, data_off = 52, 0x100, 0x120
    sym_off = 0x140
    str_off = sym_off + len(syms)
    sh_off = (str_off + len(strtab) + 3) & ~3
    img = bytearray(sh_off + 4 * 40)
    img[0:16] = b"\x7fELF\x01\x02\x01" + b"\0" * 9
    struct.pack_into(">HHIIIIIHHHHHH", img, 16, 2, 20, 1, 0x80003100, ph_off, sh_off, 0, 52, 32, 2,
                     40, 4, 0)
    struct.pack_into(">IIIIIIII", img, ph_off, PT_LOAD, text_off, 0x80003100, 0, 8, 8, 5, 32)
    struct.pack_into(">IIIIIIII", img, ph_off + 32, PT_LOAD, data_off, 0x80004000, 0, 3, 0x40, 6, 32)
    img[text_off:text_off + 8] = text
    img[data_off:data_off + 3] = datab
    img[sym_off:sym_off + len(syms)] = syms
    img[str_off:str_off + len(strtab)] = strtab
    struct.pack_into(">IIIIIIIIII", img, sh_off + 2 * 40, 0, SHT_SYMTAB, 0, 0, sym_off, len(syms), 3, 0, 4, 16)
    struct.pack_into(">IIIIIIIIII", img, sh_off + 3 * 40, 0, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0)

    entry, segs, symbols = parse_elf(bytes(img))
    dol = build_dol(entry, segs, bytes(img))
    check(len(dol) == HEADER + 32 + 32, "tamaño del DOL")
    check(struct.unpack_from(">III", dol, 0) == (HEADER, 0, 0), "offset del texto")
    check(struct.unpack_from(">I", dol, 0x48)[0] == 0x80003100, "dirección del texto")
    check(struct.unpack_from(">I", dol, 0x90)[0] == 32, "texto redondeado a 32")
    check(struct.unpack_from(">I", dol, 0x00 + 4 * 7)[0] == HEADER + 32, "offset de los datos")
    check(struct.unpack_from(">III", dol, 0xD8) == (0x80004003, 0x3D, 0x80003100), "BSS y entrada")
    check(dol[HEADER:HEADER + 8] == text and dol[HEADER + 32:HEADER + 35] == datab, "contenido")
    m = build_map(symbols)
    check("80003100 000008 80003100 0 main" in m and "80004000 000003 80004000 0 valor" in m, "mapa")
    with tempfile.TemporaryDirectory() as tmp:
        e, d, mp = (os.path.join(tmp, n) for n in ("a.elf", "a.dol", "a.map"))
        with open(e, "wb") as f:
            f.write(img)
        convert(e, d, mp)
        check(open(d, "rb").read() == dol, "archivo DOL")
    try:
        parse_elf(b"\x7fELF\x01\x01" + b"\0" * 60)
        check(False, "acepta little-endian")
    except ElfError:
        pass
    print("elf2dol selftest: OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description="ELF de PowerPC a DOL de GameCube")
    ap.add_argument("elf", nargs="?")
    ap.add_argument("dol", nargs="?")
    ap.add_argument("--map", help="escribir también el .map (formato de Dolphin)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.elf or not args.dol:
        ap.error("faltan el ELF y el DOL")
    try:
        convert(args.elf, args.dol, args.map)
    except (ElfError, OSError) as e:
        print("elf2dol: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
