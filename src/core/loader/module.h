#pragma once

// One loaded PS4 executable or PRX: its segments mapped at their guest
// address, plus the parsed Sony dynamic tables (symbols, relocations,
// imported and exported libraries).

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/loader/elf_types.h"

namespace Core::Loader {

struct LibraryRef {
    std::string name;
    u16 id;
};

class Module {
public:
    // Maps the image at `base` (must be free). Returns null and fills `error`
    // on failure.
    static std::unique_ptr<Module> Load(const std::filesystem::path& path, VAddr base,
                                        std::string& error);
    ~Module();

    const std::string& Name() const { return name; }
    VAddr Base() const { return base; }
    u64 Size() const { return size; }
    VAddr Entry() const { return base + entry; }
    bool IsExecutable() const { return type == ET_SCE_EXEC || type == ET_SCE_DYNEXEC; }

    // Guest addresses of the module's init code (DT_INIT; may sit at offset 0)
    // and of its proc/module param.
    bool HasInit() const { return has_init; }
    VAddr InitAddr() const { return base + init; }
    VAddr ProcParam() const { return proc_param ? base + proc_param : 0; }

    // Executable range, for the instruction patcher.
    VAddr CodeBegin() const { return base + code_begin; }
    VAddr CodeEnd() const { return base + code_end; }

    // TLS template (PT_TLS); tls_index is assigned by the linker.
    bool HasTls() const { return tls.memsz != 0; }
    VAddr TlsImage() const { return base + tls.vaddr; }
    u64 TlsFileSize() const { return tls.filesz; }
    u64 TlsMemSize() const { return tls.memsz; }
    u64 TlsAlign() const { return tls.align; }
    u32 tls_index = 0;

    std::span<const Elf64_Sym> Symbols() const;
    std::span<const Elf64_Rela> Relocations() const;
    std::span<const Elf64_Rela> JumpRelocations() const;
    std::string_view String(u32 offset) const;

    const std::vector<LibraryRef>& ImportLibs() const { return import_libs; }
    const std::vector<LibraryRef>& ExportLibs() const { return export_libs; }
    const std::vector<std::string>& NeededModules() const { return needed_modules; }

    // Applies segment permissions (R/W/X) once relocation is done.
    void Protect();

private:
    Module() = default;
    bool MapSegments(const class ElfImage& image, std::string& error);
    bool ParseDynamic(const class ElfImage& image, std::string& error);

    std::string name;
    u16 type = 0;
    VAddr base = 0;
    u64 size = 0;
    u64 entry = 0;
    u64 init = 0;
    bool has_init = false;
    u64 code_begin = 0;
    u64 code_end = 0;
    u64 proc_param = 0;

    struct {
        u64 vaddr = 0, filesz = 0, memsz = 0, align = 0;
    } tls;

    struct Range {
        u64 offset = 0, size = 0;
    };
    // Copy of PT_SCE_DYNLIBDATA: the tables below index into it.
    std::vector<u8> dynlib;
    Range strtab, symtab, rela, jmprel;

    std::vector<LibraryRef> import_libs;
    std::vector<LibraryRef> export_libs;
    std::vector<std::string> needed_modules;
    std::vector<Elf64_Phdr> load_segments;
};

} // namespace Core::Loader
