#pragma once

// Registry of our host implementations of Sony system library functions.
// PS4 and macOS share the x86-64 System V ABI, so an HLE function is a plain
// C++ function with the guest signature; no ABI thunks are needed.

#include <string_view>

namespace Hle {

// Looks up an implementation by library name and NID ("NID" = the 11-char
// hashed symbol name used by Sony's linker). Returns null if not implemented.
void* Lookup(std::string_view library, std::string_view nid);

void Register(std::string_view library, std::string_view nid, void* function);

} // namespace Hle
