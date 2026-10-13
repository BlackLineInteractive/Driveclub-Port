#!/usr/bin/env python3
"""Convert a decrypted PS4 (f)SELF into a plain ELF for disassemblers.

The output keeps the original program headers and adds section headers for
the code and data segments, so llvm-objdump / Ghidra / IDA can load it.
Sony e_type values are rewritten to ET_DYN (addresses stay module-relative).

Usage:
    tools/self2elf.py extracted/Image0/eboot.bin out/eboot.elf
    llvm-objdump -d --start-address=0x486600 --stop-address=0x486720 out/eboot.elf
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfinfo import Self  # noqa: E402

PT_LOAD = 1
PT_SCE_RELRO = 0x61000010
SHT_PROGBITS = 1
SHT_STRTAB = 3
SHT_NOBITS = 8
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        s = Self(f.read())

    elf_off = 32 + struct.unpack_from("<H", s.data, 24)[0] * 32
    ehdr = bytearray(s.data[elf_off:elf_off + 64])
    phnum = struct.unpack_from("<H", ehdr, 0x38)[0]
    phdr_bytes = s.data[elf_off + 64:elf_off + 64 + phnum * 56]

    # Lay segments out at their original file offsets.
    end = 64 + phnum * 56
    blobs = []
    for idx, ph in enumerate(s.phdrs):
        if idx in s.blobs:
            blobs.append((ph[2], s.blobs[idx]))
            end = max(end, ph[2] + len(s.blobs[idx]))
    out = bytearray(end)
    out[64:64 + len(phdr_bytes)] = phdr_bytes
    for off, blob in blobs:
        out[off:off + len(blob)] = blob

    names = bytearray(b"\0")
    sections = [bytes(64)]  # SHT_NULL

    def add_section(name, sh_type, flags, addr, offset, size):
        name_off = len(names)
        names.extend(name.encode() + b"\0")
        sections.append(struct.pack("<IIQQQQIIQQ", name_off, sh_type, flags, addr, offset,
                                    size, 0, 0, 16, 0))

    for idx, (ptype, pflags, poff, vaddr, _, filesz, memsz, _) in enumerate(s.phdrs):
        if ptype not in (PT_LOAD, PT_SCE_RELRO) or idx not in s.blobs:
            continue
        if pflags & 1:
            add_section(f".text.{idx}", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, vaddr, poff,
                        filesz)
        else:
            add_section(f".data.{idx}", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, vaddr, poff, filesz)
            if memsz > filesz:
                add_section(f".bss.{idx}", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, vaddr + filesz,
                            poff + filesz, memsz - filesz)

    shstrtab_name = len(names)
    names.extend(b".shstrtab\0")
    names_off = len(out)
    out.extend(names)
    sections.append(struct.pack("<IIQQQQIIQQ", shstrtab_name, SHT_STRTAB, 0, 0, names_off,
                                len(names), 0, 0, 1, 0))

    while len(out) % 8:
        out.append(0)
    shoff = len(out)
    for sec in sections:
        out.extend(sec)

    struct.pack_into("<H", ehdr, 0x10, 3)  # ET_DYN
    struct.pack_into("<Q", ehdr, 0x20, 64)  # e_phoff
    struct.pack_into("<Q", ehdr, 0x28, shoff)
    struct.pack_into("<HHH", ehdr, 0x3A, 64, len(sections), len(sections) - 1)
    out[0:64] = ehdr

    os.makedirs(os.path.dirname(os.path.abspath(sys.argv[2])), exist_ok=True)
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    print(f"wrote {sys.argv[2]} ({len(out)} bytes, {len(sections)} sections)")


if __name__ == "__main__":
    main()
