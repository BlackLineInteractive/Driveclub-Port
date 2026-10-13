#pragma once

// Registry of our host implementations of Sony system library functions.
// PS4 and macOS share the x86-64 System V ABI, so an HLE function is a plain
// C++ function with the guest signature; no ABI thunks are needed.

#include <string>
#include <string_view>

namespace Hle {

// Sony's linker identifies symbols by NID: 11 chars of base64 over the first
// 8 bytes of SHA-1(name + fixed salt).
std::string Nid(std::string_view symbol_name);

// Human-readable name for a NID the game imports, or the NID itself.
std::string_view NameOf(std::string_view nid);

// Looks up an implementation by library name and NID. Null if not implemented.
void* Lookup(std::string_view library, std::string_view nid);

void Register(std::string_view library, std::string_view nid, void* function);

// Registers every HLE library; call once before linking.
void RegisterAll();

} // namespace Hle

// Registers `fn` under its own name in `library`: DC_HLE("libkernel", sceKernelUsleep);
#define DC_HLE(library, fn) ::Hle::Register(library, ::Hle::Nid(#fn), reinterpret_cast<void*>(&fn))
// Same, for a C++ function whose guest name differs (e.g. guest "open" -> posix_open).
#define DC_HLE_AS(library, name, fn)                                                               \
    ::Hle::Register(library, ::Hle::Nid(name), reinterpret_cast<void*>(&fn))
