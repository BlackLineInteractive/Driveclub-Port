#pragma once

// Reads a PS4 executable or module from disk: either a decrypted (f)SELF or a
// plain ELF. The result gives the ELF headers and the bytes behind each
// program header.

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "core/loader/elf_types.h"

namespace Core::Loader {

class ElfImage {
public:
    // Returns false and fills `error` if the file is not a usable image.
    bool Open(const std::filesystem::path& path, std::string& error);

    const Elf64_Ehdr& Header() const { return ehdr; }
    std::span<const Elf64_Phdr> ProgramHeaders() const { return phdrs; }

    // File bytes of program header `index` (p_filesz long), or empty if the
    // image does not carry them.
    std::span<const u8> SegmentData(size_t index) const;

private:
    struct Blob {
        u64 elf_offset; // p_offset of the program header it backs
        u64 file_offset;
        u64 size;
    };

    std::vector<u8> file;
    Elf64_Ehdr ehdr{};
    std::vector<Elf64_Phdr> phdrs;
    // SELF: where each program header's bytes sit in the file. Plain ELF: empty.
    std::vector<Blob> blobs;
    bool is_self = false;
};

} // namespace Core::Loader
