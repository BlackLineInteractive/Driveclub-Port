#pragma once

// Loads the game's executable and modules at their guest addresses and binds
// every import to, in order: an export of another loaded module, an HLE
// implementation, or a trap stub that reports the missing function.

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/loader/module.h"

namespace Core::Loader {

class Linker {
public:
    Linker();
    ~Linker();

    // Loads eboot.bin, the game's own *.prx and sce_module/*.prx from `game_dir`.
    bool LoadGame(const std::filesystem::path& game_dir);

    // Applies relocations of all modules, then sets final page protections.
    void Relocate();

    void PrintReport() const;

    const Module* Executable() const { return modules.empty() ? nullptr : modules[0].get(); }
    const std::vector<std::unique_ptr<Module>>& Modules() const { return modules; }
    Module* FindModule(std::string_view name) const;

    // "module+0xoffset" for an address inside a loaded module, else hex.
    std::string Describe(u64 address) const;

    // The single linker instance, for signal handlers and HLE code.
    static Linker* Instance() { return instance; }

private:
    struct Unresolved {
        std::string library;
        std::string nid;
        std::string first_user; // module that imports it
        u32 calls = 0;
    };

    bool LoadModule(const std::filesystem::path& path, VAddr base);
    void CollectExports(const Module& m);
    VAddr ResolveImport(const Module& m, std::string_view symbol_name);
    VAddr MakeStub(const std::string& library, const std::string& nid, const Module& user);
    void RelocateModule(Module& m);

    std::vector<std::unique_ptr<Module>> modules;
    std::unordered_map<std::string, VAddr> exports; // "library:NID" -> address
    std::unordered_map<std::string, VAddr> stubs;   // "library:NID" -> trap stub

    // Executable page holding the trap stubs.
    u8* stub_code = nullptr;
    size_t stub_capacity = 0;

    struct Stats {
        u64 relocations = 0;
        u64 bound_to_module = 0;
        u64 bound_to_hle = 0;
        u64 bound_to_stub = 0;
        std::map<u32, u64> by_type;
        std::map<u32, u64> unknown_types;
    } stats;

    static inline Linker* instance = nullptr;

public:
    // Read by the trap handler; index = stub number.
    static inline std::vector<Unresolved> unresolved;
};

} // namespace Core::Loader
