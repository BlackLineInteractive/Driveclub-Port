#!/usr/bin/env python3
"""Inspect a PS4 (f)SELF: program headers, needed modules, imports per library.

Usage:
    tools/elfinfo.py extracted/Image0/eboot.bin
    tools/elfinfo.py extracted/Image0/eboot.bin --lib libSceGnmDriver   # list NIDs
"""

import argparse
import collections
import struct
import sys

PT_DYNAMIC = 0x2
PT_SCE_DYNLIBDATA = 0x61000000

DT_NEEDED = 0x1
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_STRTAB = 0x61000035
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F

NID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

SEG_ENCRYPTED = 0x2
SEG_BLOCKED = 0x800


def decode_index(text):
    value = 0
    for c in text:
        value = value * 64 + NID_ALPHABET.index(c)
    return value


class Self:
    def __init__(self, data):
        self.data = data
        if struct.unpack_from("<I", data, 0)[0] != 0x1D3D154F:
            sys.exit("not a SELF file")
        seg_count = struct.unpack_from("<H", data, 24)[0]

        # Blocked SELF segments carry the bytes of ELF program header #id.
        self.blobs = {}
        self.encrypted = False
        for i in range(seg_count):
            flags, offset, size, _ = struct.unpack_from("<QQQQ", data, 32 + i * 32)
            self.encrypted |= bool(flags & SEG_ENCRYPTED)
            if flags & SEG_BLOCKED:
                self.blobs[(flags >> 20) & 0xFFF] = data[offset:offset + size]

        elf = 32 + seg_count * 32
        self.entry = struct.unpack_from("<Q", data, elf + 0x18)[0]
        phoff = struct.unpack_from("<Q", data, elf + 0x20)[0]
        phnum = struct.unpack_from("<H", data, elf + 0x38)[0]
        self.phdrs = [struct.unpack_from("<IIQQQQQQ", data, elf + phoff + i * 56)
                      for i in range(phnum)]

    def phdr(self, ptype):
        for i, ph in enumerate(self.phdrs):
            if ph[0] == ptype:
                return i, ph
        sys.exit(f"missing program header {ptype:#x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("path")
    ap.add_argument("--lib", help="print the NIDs imported from this library")
    args = ap.parse_args()

    with open(args.path, "rb") as f:
        s = Self(f.read())
    if s.encrypted:
        sys.exit("segments are encrypted: a decrypted dump is required")

    print(f"entry {s.entry:#x}")
    for ptype, flags, _, vaddr, _, filesz, memsz, _ in s.phdrs:
        print(f"  phdr {ptype:#010x} flags {flags} va {vaddr:#x} filesz {filesz:#x} memsz {memsz:#x}")

    idx, dynlib_ph = s.phdr(PT_SCE_DYNLIBDATA)
    dynlib = s.blobs[idx]
    _, dyn_ph = s.phdr(PT_DYNAMIC)
    start = dyn_ph[2] - dynlib_ph[2]  # PT_DYNAMIC lives inside the dynlib blob
    dyn = dynlib[start:start + dyn_ph[5]]

    tags = collections.defaultdict(list)
    for off in range(0, len(dyn), 16):
        tag, val = struct.unpack_from("<qQ", dyn, off)
        tags[tag].append(val)

    strtab = tags[DT_SCE_STRTAB][0]

    def string(off):
        begin = strtab + off
        return dynlib[begin:dynlib.index(b"\0", begin)].decode()

    libs = {v >> 48: string(v & 0xFFFFFFFF) for v in tags[DT_SCE_IMPORT_LIB]}
    print("needed:", ", ".join(string(v) for v in tags[DT_NEEDED]))

    symtab = tags[DT_SCE_SYMTAB][0]
    symsz = tags[DT_SCE_SYMTABSZ][0]
    counts = collections.Counter()
    for off in range(0, symsz, 24):
        name_off, _, _, shndx, _, _ = struct.unpack_from("<IBBHQQ", dynlib, symtab + off)
        name = string(name_off)
        if shndx != 0 or name.count("#") != 2:
            continue
        nid, lib, _ = name.split("#")
        lib_name = libs.get(decode_index(lib), lib)
        counts[lib_name] += 1
        if args.lib == lib_name:
            print("NID", nid)

    if not args.lib:
        print("imports per library:")
        for lib, n in counts.most_common():
            print(f"  {n:5d} {lib}")
        print(f"total {sum(counts.values())}")


if __name__ == "__main__":
    main()
