#include "core/loader/self.h"

#include <cstring>
#include <fstream>

namespace Core::Loader {

bool ElfImage::Open(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "cannot open file";
        return false;
    }
    file.resize(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));

    if (file.size() < sizeof(SelfHeader)) {
        error = "file too small";
        return false;
    }

    u32 magic;
    std::memcpy(&magic, file.data(), sizeof(magic));

    size_t elf_offset = 0;
    std::vector<SelfSegment> self_segments;
    if (magic == kSelfMagic) {
        is_self = true;
        SelfHeader self;
        std::memcpy(&self, file.data(), sizeof(self));
        self_segments.resize(self.segment_count);
        std::memcpy(self_segments.data(), file.data() + sizeof(SelfHeader),
                    self.segment_count * sizeof(SelfSegment));
        elf_offset = sizeof(SelfHeader) + self.segment_count * sizeof(SelfSegment);
    } else if (magic != kElfMagic) {
        error = "not a SELF or ELF file";
        return false;
    }

    std::memcpy(&ehdr, file.data() + elf_offset, sizeof(ehdr));
    if (std::memcmp(ehdr.e_ident, "\x7F" "ELF", 4) != 0) {
        error = "missing ELF header";
        return false;
    }
    phdrs.resize(ehdr.e_phnum);
    std::memcpy(phdrs.data(), file.data() + elf_offset + ehdr.e_phoff,
                ehdr.e_phnum * sizeof(Elf64_Phdr));

    for (const SelfSegment& seg : self_segments) {
        if (!seg.IsBlocked()) {
            continue; // signatures / hash tables, not program data
        }
        if (seg.IsEncrypted()) {
            error = "segments are encrypted: a decrypted dump is required";
            return false;
        }
        if (seg.IsCompressed()) {
            error = "compressed SELF segments are not supported";
            return false;
        }
        if (seg.Id() >= phdrs.size() || seg.file_offset + seg.file_size > file.size()) {
            error = "corrupt SELF segment table";
            return false;
        }
        blobs.push_back({phdrs[seg.Id()].p_offset, seg.file_offset, seg.file_size});
    }
    return true;
}

std::span<const u8> ElfImage::SegmentData(size_t index) const {
    const Elf64_Phdr& ph = phdrs[index];
    if (ph.p_filesz == 0) {
        return {};
    }
    if (!is_self) {
        if (ph.p_offset + ph.p_filesz > file.size()) {
            return {};
        }
        return {file.data() + ph.p_offset, ph.p_filesz};
    }
    // Program headers without their own SELF segment (e.g. PT_DYNAMIC) live
    // inside another one's bytes, so locate them by ELF file offset.
    for (const Blob& blob : blobs) {
        if (ph.p_offset >= blob.elf_offset &&
            ph.p_offset + ph.p_filesz <= blob.elf_offset + blob.size) {
            return {file.data() + blob.file_offset + (ph.p_offset - blob.elf_offset),
                    ph.p_filesz};
        }
    }
    return {};
}

} // namespace Core::Loader
