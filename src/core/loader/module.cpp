#include "core/loader/module.h"

#include <cstring>

#include <sys/mman.h>

#include "common/log.h"
#include "core/address_space.h"
#include "core/loader/self.h"

namespace Core::Loader {

namespace {

constexpr u64 kPageSize = 0x4000; // PS4 page size; also a multiple of host 4K pages

bool IsLoadable(const Elf64_Phdr& ph) {
    return ph.p_type == PT_LOAD || ph.p_type == PT_SCE_RELRO;
}

// Dynamic tags pack (name offset, version, id) into one value.
LibraryRef DecodeRef(const Module& m, u64 value) {
    return {std::string(m.String(static_cast<u32>(value))), static_cast<u16>(value >> 48)};
}

} // namespace

std::unique_ptr<Module> Module::Load(const std::filesystem::path& path, VAddr base,
                                     std::string& error) {
    ElfImage image;
    if (!image.Open(path, error)) {
        return nullptr;
    }

    std::unique_ptr<Module> m(new Module());
    m->name = path.stem().string();
    m->type = image.Header().e_type;
    m->entry = image.Header().e_entry;
    m->base = base;

    if (!m->ParseDynamic(image, error) || !m->MapSegments(image, error)) {
        return nullptr;
    }
    return m;
}

Module::~Module() {
    if (size != 0) {
        AddressSpace::Unmap(base, size);
    }
}

bool Module::MapSegments(const ElfImage& image, std::string& error) {
    const auto phdrs = image.ProgramHeaders();

    u64 end = 0;
    for (const Elf64_Phdr& ph : phdrs) {
        if (IsLoadable(ph)) {
            end = std::max(end, ph.p_vaddr + ph.p_memsz);
        }
    }
    size = AlignUp(end, kPageSize);

    // Guest addresses are host addresses, so the module lands exactly at
    // `base`, inside the guest ranges reserved at process start.
    if (!AddressSpace::Map(base, size, AddressSpace::Prot::ReadWrite)) {
        size = 0;
        error = "cannot map guest address range";
        return false;
    }

    for (size_t i = 0; i < phdrs.size(); ++i) {
        const Elf64_Phdr& ph = phdrs[i];
        switch (ph.p_type) {
        case PT_LOAD:
        case PT_SCE_RELRO: {
            const auto data = image.SegmentData(i);
            if (data.size() != ph.p_filesz) {
                error = "segment data missing from image";
                return false;
            }
            std::memcpy(reinterpret_cast<void*>(base + ph.p_vaddr), data.data(), data.size());
            load_segments.push_back(ph);
            if (ph.p_flags & PF_X) {
                code_begin = ph.p_vaddr;
                code_end = ph.p_vaddr + ph.p_memsz;
            }
            break;
        }
        case PT_TLS:
            tls = {ph.p_vaddr, ph.p_filesz, ph.p_memsz, ph.p_align};
            break;
        case PT_SCE_PROCPARAM:
        case PT_SCE_MODULE_PARAM:
            proc_param = ph.p_vaddr;
            break;
        default:
            break;
        }
    }
    return true;
}

bool Module::ParseDynamic(const ElfImage& image, std::string& error) {
    std::span<const u8> dynamic;
    const auto phdrs = image.ProgramHeaders();
    for (size_t i = 0; i < phdrs.size(); ++i) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dynamic = image.SegmentData(i);
        } else if (phdrs[i].p_type == PT_SCE_DYNLIBDATA) {
            const auto data = image.SegmentData(i);
            dynlib.assign(data.begin(), data.end());
        }
    }
    if (dynamic.empty() || dynlib.empty()) {
        error = "missing dynamic tables";
        return false;
    }

    std::vector<Elf64_Dyn> tags(dynamic.size() / sizeof(Elf64_Dyn));
    std::memcpy(tags.data(), dynamic.data(), tags.size() * sizeof(Elf64_Dyn));

    // String table first: the library tags reference it.
    for (const Elf64_Dyn& d : tags) {
        if (d.d_tag == DT_SCE_STRTAB) {
            strtab.offset = d.d_val;
        } else if (d.d_tag == DT_SCE_STRSZ) {
            strtab.size = d.d_val;
        }
    }

    for (const Elf64_Dyn& d : tags) {
        switch (d.d_tag) {
        case DT_SCE_SYMTAB:
            symtab.offset = d.d_val;
            break;
        case DT_SCE_SYMTABSZ:
            symtab.size = d.d_val;
            break;
        case DT_SCE_RELA:
            rela.offset = d.d_val;
            break;
        case DT_SCE_RELASZ:
            rela.size = d.d_val;
            break;
        case DT_SCE_JMPREL:
            jmprel.offset = d.d_val;
            break;
        case DT_SCE_PLTRELSZ:
            jmprel.size = d.d_val;
            break;
        case DT_INIT:
            init = d.d_val;
            has_init = true;
            break;
        case DT_SCE_IMPORT_LIB:
            import_libs.push_back(DecodeRef(*this, d.d_val));
            break;
        case DT_SCE_EXPORT_LIB:
            export_libs.push_back(DecodeRef(*this, d.d_val));
            break;
        case DT_SCE_NEEDED_MODULE:
            needed_modules.push_back(DecodeRef(*this, d.d_val).name);
            break;
        default:
            break;
        }
    }

    for (const Range* r : {&strtab, &symtab, &rela, &jmprel}) {
        if (r->offset + r->size > dynlib.size()) {
            error = "dynamic table out of bounds";
            return false;
        }
    }
    return true;
}

std::span<const Elf64_Sym> Module::Symbols() const {
    return {reinterpret_cast<const Elf64_Sym*>(dynlib.data() + symtab.offset),
            symtab.size / sizeof(Elf64_Sym)};
}

std::span<const Elf64_Rela> Module::Relocations() const {
    return {reinterpret_cast<const Elf64_Rela*>(dynlib.data() + rela.offset),
            rela.size / sizeof(Elf64_Rela)};
}

std::span<const Elf64_Rela> Module::JumpRelocations() const {
    return {reinterpret_cast<const Elf64_Rela*>(dynlib.data() + jmprel.offset),
            jmprel.size / sizeof(Elf64_Rela)};
}

std::string_view Module::String(u32 offset) const {
    if (offset >= strtab.size) {
        return {};
    }
    const char* s = reinterpret_cast<const char*>(dynlib.data() + strtab.offset + offset);
    return {s, strnlen(s, strtab.size - offset)};
}

void Module::Protect() {
    for (const Elf64_Phdr& ph : load_segments) {
        int prot = 0;
        prot |= (ph.p_flags & PF_R) ? PROT_READ : 0;
        prot |= (ph.p_flags & PF_W) ? PROT_WRITE : 0;
        prot |= (ph.p_flags & PF_X) ? PROT_EXEC | PROT_READ : 0;
        const u64 start = AlignDown(base + ph.p_vaddr, kPageSize);
        const u64 end = AlignUp(base + ph.p_vaddr + ph.p_memsz, kPageSize);
        if (mprotect(reinterpret_cast<void*>(start), end - start, prot) != 0) {
            DC_WARN("%s: mprotect %#llx-%#llx failed", name.c_str(), start, end);
        }
    }
}

} // namespace Core::Loader
