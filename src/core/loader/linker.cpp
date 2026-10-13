#include "core/loader/linker.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>

#include "common/log.h"
#include "hle/hle.h"

namespace Core::Loader {

namespace {

// Same layout the PS4 kernel uses: the executable low, game modules at 2 GB,
// Sony system modules at 32 GB.
constexpr VAddr kExecutableBase = 0x400000;
constexpr VAddr kGameModuleBase = 0x80000000;
constexpr VAddr kSystemModuleBase = 0x800000000;
// Room after each module for future code-patch trampolines.
constexpr u64 kModuleGap = 16ull << 20;

constexpr size_t kStubSize = 32;
constexpr size_t kMaxStubs = 8192;

constexpr std::string_view kNidAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

// Symbol names have the form "NID#L#M": L and M are base64 library/module ids.
struct SymbolName {
    std::string_view nid;
    u16 library_id;
};

bool ParseSymbolName(std::string_view name, SymbolName& out) {
    const size_t a = name.find('#');
    const size_t b = name.find('#', a + 1);
    if (a == std::string_view::npos || b == std::string_view::npos) {
        return false;
    }
    u32 id = 0;
    for (char c : name.substr(a + 1, b - a - 1)) {
        const size_t digit = kNidAlphabet.find(c);
        if (digit == std::string_view::npos) {
            return false;
        }
        id = id * 64 + static_cast<u32>(digit);
    }
    out = {name.substr(0, a), static_cast<u16>(id)};
    return true;
}

const LibraryRef* FindLib(const std::vector<LibraryRef>& libs, u16 id) {
    for (const LibraryRef& lib : libs) {
        if (lib.id == id) {
            return &lib;
        }
    }
    return nullptr;
}

std::string ExportKey(std::string_view library, std::string_view nid) {
    std::string key(library);
    key.append(":").append(nid);
    return key;
}

std::vector<std::filesystem::path> ListPrx(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const auto ext = entry.path().extension();
        if (entry.is_regular_file() && (ext == ".prx" || ext == ".sprx")) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

// Target of every trap stub: the game called a function nobody implements.
extern "C" [[noreturn]] void DcUnresolvedImport(u32 index) {
    const auto& u = Linker::unresolved[index];
    DC_ERROR("unimplemented import called: %s:%s (imported by %s)", u.library.c_str(),
             u.nid.c_str(), u.first_user.c_str());
    std::abort();
}

Linker::Linker() {
    stub_capacity = kMaxStubs;
    void* mem = mmap(nullptr, kStubSize * kMaxStubs, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    stub_code = mem == MAP_FAILED ? nullptr : static_cast<u8*>(mem);
}

Linker::~Linker() {
    if (stub_code) {
        munmap(stub_code, kStubSize * kMaxStubs);
    }
}

bool Linker::LoadModule(const std::filesystem::path& path, VAddr base) {
    std::string error;
    auto m = Module::Load(path, base, error);
    if (!m) {
        DC_ERROR("failed to load %s: %s", path.filename().c_str(), error.c_str());
        return false;
    }
    DC_INFO("loaded %-20s at %#011llx size %#llx", m->Name().c_str(), m->Base(), m->Size());
    CollectExports(*m);
    modules.push_back(std::move(m));
    return true;
}

bool Linker::LoadGame(const std::filesystem::path& game_dir) {
    if (!LoadModule(game_dir / "eboot.bin", kExecutableBase)) {
        return false;
    }

    VAddr next = kGameModuleBase;
    for (const auto& prx : ListPrx(game_dir)) {
        if (!LoadModule(prx, next)) {
            return false;
        }
        next = AlignUp(modules.back()->Base() + modules.back()->Size() + kModuleGap, 1 << 20);
    }

    next = kSystemModuleBase;
    for (const auto& prx : ListPrx(game_dir / "sce_module")) {
        if (!LoadModule(prx, next)) {
            return false;
        }
        next = AlignUp(modules.back()->Base() + modules.back()->Size() + kModuleGap, 1 << 20);
    }

    // Module TLS indices start at 1, in load order.
    u32 tls_index = 1;
    for (auto& m : modules) {
        if (m->HasTls()) {
            m->tls_index = tls_index++;
        }
    }
    return true;
}

void Linker::CollectExports(const Module& m) {
    for (const Elf64_Sym& sym : m.Symbols()) {
        if (sym.st_shndx == SHN_UNDEF || sym.Binding() == STB_LOCAL) {
            continue;
        }
        SymbolName sn;
        if (!ParseSymbolName(m.String(sym.st_name), sn)) {
            continue;
        }
        if (const LibraryRef* lib = FindLib(m.ExportLibs(), sn.library_id)) {
            exports.emplace(ExportKey(lib->name, sn.nid), m.Base() + sym.st_value);
        }
    }
}

VAddr Linker::MakeStub(const std::string& library, const std::string& nid, const Module& user) {
    const std::string key = ExportKey(library, nid);
    if (const auto it = stubs.find(key); it != stubs.end()) {
        return it->second;
    }
    const u32 index = static_cast<u32>(unresolved.size());
    if (!stub_code || index >= stub_capacity) {
        DC_ERROR("out of trap stubs");
        std::abort();
    }
    unresolved.push_back({library, nid, user.Name()});

    // mov edi, index ; movabs rax, DcUnresolvedImport ; jmp rax
    u8* p = stub_code + index * kStubSize;
    const u64 handler = reinterpret_cast<u64>(&DcUnresolvedImport);
    p[0] = 0xBF;
    std::memcpy(p + 1, &index, 4);
    p[5] = 0x48;
    p[6] = 0xB8;
    std::memcpy(p + 7, &handler, 8);
    p[15] = 0xFF;
    p[16] = 0xE0;

    const VAddr addr = reinterpret_cast<VAddr>(p);
    stubs.emplace(key, addr);
    return addr;
}

VAddr Linker::ResolveImport(const Module& m, std::string_view symbol_name) {
    SymbolName sn;
    if (!ParseSymbolName(symbol_name, sn)) {
        return 0;
    }
    const LibraryRef* lib = FindLib(m.ImportLibs(), sn.library_id);
    if (!lib) {
        return 0;
    }
    if (const auto it = exports.find(ExportKey(lib->name, sn.nid)); it != exports.end()) {
        ++stats.bound_to_module;
        return it->second;
    }
    if (void* fn = Hle::Lookup(lib->name, sn.nid)) {
        ++stats.bound_to_hle;
        return reinterpret_cast<VAddr>(fn);
    }
    ++stats.bound_to_stub;
    return MakeStub(lib->name, std::string(sn.nid), m);
}

void Linker::RelocateModule(Module& m) {
    const auto symbols = m.Symbols();

    auto apply = [&](const Elf64_Rela& r) {
        ++stats.relocations;
        ++stats.by_type[r.Type()];

        auto* target = reinterpret_cast<u64*>(m.Base() + r.r_offset);
        const Elf64_Sym* sym = r.Symbol() < symbols.size() ? &symbols[r.Symbol()] : nullptr;

        // Address of the referenced symbol: defined here, or imported.
        auto symbol_addr = [&]() -> VAddr {
            if (!sym) {
                return 0;
            }
            if (sym->st_shndx != SHN_UNDEF) {
                return m.Base() + sym->st_value;
            }
            return ResolveImport(m, m.String(sym->st_name));
        };

        switch (r.Type()) {
        case R_X86_64_NONE:
            break;
        case R_X86_64_RELATIVE:
            *target = m.Base() + r.r_addend;
            break;
        case R_X86_64_64:
            *target = symbol_addr() + r.r_addend;
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *target = symbol_addr();
            break;
        case R_X86_64_DTPMOD64:
            *target = m.tls_index;
            break;
        case R_X86_64_DTPOFF64:
            *target = (sym ? sym->st_value : 0) + r.r_addend;
            break;
        default:
            ++stats.unknown_types[r.Type()];
            break;
        }
    };

    for (const Elf64_Rela& r : m.Relocations()) {
        apply(r);
    }
    for (const Elf64_Rela& r : m.JumpRelocations()) {
        apply(r);
    }
}

void Linker::Relocate() {
    for (auto& m : modules) {
        RelocateModule(*m);
    }
    for (auto& m : modules) {
        m->Protect();
    }
    if (stub_code) {
        mprotect(stub_code, kStubSize * kMaxStubs, PROT_READ | PROT_EXEC);
    }
}

void Linker::PrintReport() const {
    DC_INFO("modules: %zu, exports: %zu", modules.size(), exports.size());
    for (const auto& m : modules) {
        DC_INFO("  %-20s base %#011llx entry %#011llx init %#011llx tls %u imports-libs %zu",
                m->Name().c_str(), m->Base(), m->Entry(), m->InitAddr(), m->tls_index,
                m->ImportLibs().size());
    }
    DC_INFO("relocations: %llu (to modules %llu, to hle %llu, to stubs %llu)", stats.relocations,
            stats.bound_to_module, stats.bound_to_hle, stats.bound_to_stub);
    for (const auto& [type, n] : stats.by_type) {
        DC_INFO("  type %2u: %llu", type, n);
    }
    for (const auto& [type, n] : stats.unknown_types) {
        DC_WARN("  unhandled relocation type %u: %llu", type, n);
    }

    std::map<std::string, u64> per_library;
    for (const Unresolved& u : unresolved) {
        ++per_library[u.library];
    }
    std::vector<std::pair<std::string, u64>> sorted(per_library.begin(), per_library.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    DC_INFO("unresolved functions: %zu (need HLE)", unresolved.size());
    for (const auto& [lib, n] : sorted) {
        DC_INFO("  %5llu %s", n, lib.c_str());
    }
}

} // namespace Core::Loader
