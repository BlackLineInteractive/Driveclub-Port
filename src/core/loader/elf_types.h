#pragma once

// ELF64 structures plus the Sony extensions used by PS4 executables and PRX
// modules. macOS has no <elf.h>, so the subset we need is defined here.

#include "common/types.h"

namespace Core::Loader {

constexpr u32 kSelfMagic = 0x1D3D154F;
constexpr u32 kElfMagic = 0x464C457F;

struct SelfHeader {
    u32 magic;
    u8 version;
    u8 mode;
    u8 endian;
    u8 attributes;
    u32 key_type;
    u16 header_size;
    u16 meta_size;
    u64 file_size;
    u16 segment_count;
    u16 unknown;
    u32 padding;
};
static_assert(sizeof(SelfHeader) == 32);

struct SelfSegment {
    u64 flags;
    u64 file_offset;
    u64 file_size;
    u64 memory_size;

    bool IsEncrypted() const { return flags & 0x2; }
    bool IsCompressed() const { return flags & 0x8; }
    // Blocked segments hold the bytes of ELF program header number Id().
    bool IsBlocked() const { return flags & 0x800; }
    u32 Id() const { return (flags >> 20) & 0xFFF; }
};
static_assert(sizeof(SelfSegment) == 32);

struct Elf64_Ehdr {
    u8 e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};
static_assert(sizeof(Elf64_Ehdr) == 64);

struct Elf64_Phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};
static_assert(sizeof(Elf64_Phdr) == 56);

struct Elf64_Sym {
    u32 st_name;
    u8 st_info;
    u8 st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;

    u8 Binding() const { return st_info >> 4; }
    u8 Type() const { return st_info & 0xF; }
};
static_assert(sizeof(Elf64_Sym) == 24);

struct Elf64_Rela {
    u64 r_offset;
    u64 r_info;
    s64 r_addend;

    u32 Type() const { return static_cast<u32>(r_info); }
    u32 Symbol() const { return static_cast<u32>(r_info >> 32); }
};
static_assert(sizeof(Elf64_Rela) == 24);

struct Elf64_Dyn {
    s64 d_tag;
    u64 d_val;
};
static_assert(sizeof(Elf64_Dyn) == 16);

// e_type
constexpr u16 ET_SCE_EXEC = 0xFE00;
constexpr u16 ET_SCE_DYNEXEC = 0xFE10;
constexpr u16 ET_SCE_DYNAMIC = 0xFE18;

// p_type
constexpr u32 PT_LOAD = 1;
constexpr u32 PT_DYNAMIC = 2;
constexpr u32 PT_TLS = 7;
constexpr u32 PT_GNU_EH_FRAME = 0x6474E550;
constexpr u32 PT_SCE_DYNLIBDATA = 0x61000000;
constexpr u32 PT_SCE_PROCPARAM = 0x61000001;
constexpr u32 PT_SCE_MODULE_PARAM = 0x61000002;
constexpr u32 PT_SCE_RELRO = 0x61000010;

// p_flags
constexpr u32 PF_X = 1;
constexpr u32 PF_W = 2;
constexpr u32 PF_R = 4;

// d_tag
constexpr s64 DT_NULL = 0;
constexpr s64 DT_NEEDED = 1;
constexpr s64 DT_INIT = 12;
constexpr s64 DT_FINI = 13;
constexpr s64 DT_INIT_ARRAY = 25;
constexpr s64 DT_FINI_ARRAY = 26;
constexpr s64 DT_INIT_ARRAYSZ = 27;
constexpr s64 DT_FINI_ARRAYSZ = 28;
constexpr s64 DT_PREINIT_ARRAY = 32;
constexpr s64 DT_PREINIT_ARRAYSZ = 33;
constexpr s64 DT_SCE_MODULE_INFO = 0x6100000D;
constexpr s64 DT_SCE_NEEDED_MODULE = 0x6100000F;
constexpr s64 DT_SCE_EXPORT_LIB = 0x61000013;
constexpr s64 DT_SCE_IMPORT_LIB = 0x61000015;
constexpr s64 DT_SCE_JMPREL = 0x61000029;
constexpr s64 DT_SCE_PLTRELSZ = 0x6100002D;
constexpr s64 DT_SCE_RELA = 0x6100002F;
constexpr s64 DT_SCE_RELASZ = 0x61000031;
constexpr s64 DT_SCE_STRTAB = 0x61000035;
constexpr s64 DT_SCE_STRSZ = 0x61000037;
constexpr s64 DT_SCE_SYMTAB = 0x61000039;
constexpr s64 DT_SCE_SYMTABSZ = 0x6100003F;

// Symbol binding / section
constexpr u8 STB_LOCAL = 0;
constexpr u8 STB_GLOBAL = 1;
constexpr u8 STB_WEAK = 2;
constexpr u16 SHN_UNDEF = 0;

// Relocation types
constexpr u32 R_X86_64_NONE = 0;
constexpr u32 R_X86_64_64 = 1;
constexpr u32 R_X86_64_GLOB_DAT = 6;
constexpr u32 R_X86_64_JUMP_SLOT = 7;
constexpr u32 R_X86_64_RELATIVE = 8;
constexpr u32 R_X86_64_DTPMOD64 = 16;
constexpr u32 R_X86_64_DTPOFF64 = 17;
constexpr u32 R_X86_64_TPOFF64 = 18;

} // namespace Core::Loader
