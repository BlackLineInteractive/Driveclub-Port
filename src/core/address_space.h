#pragma once

// The PS4 address ranges the game expects, reserved inside our own process.
//
// Guest addresses are used 1:1 as host addresses, so these ranges must stay
// free of anything macOS, dyld or malloc would put there. They are reserved
// as zero-fill segments of our executable (see address_space.cpp and the
// -segaddr flags in CMakeLists.txt), which the kernel maps before any library
// code runs.

#include "common/types.h"

namespace Core::AddressSpace {

// PS4 "system managed" area: executable at 0x400000, game modules at 2 GB.
constexpr VAddr kSystemManagedMin = 0x400000;
constexpr VAddr kSystemManagedMax = 0x7FFFFC000;
// PS4 "system reserved" area: Sony system modules at 32 GB.
constexpr VAddr kSystemReservedMin = 0x7FFFFC000;
constexpr VAddr kSystemReservedMax = 0xFC0000000;
// Direct and flexible memory mappings; games use fixed addresses from 64 GB up.
constexpr VAddr kUserMin = 0x1000000000;
constexpr VAddr kUserMax = 0x600000000000;

enum class Prot : int { None = 0, Read = 1, Write = 2, Exec = 4, ReadWrite = 3 };

constexpr Prot operator|(Prot a, Prot b) {
    return static_cast<Prot>(static_cast<int>(a) | static_cast<int>(b));
}

// Releases the reservations' pages and makes them inaccessible, so stray
// guest accesses fault instead of silently hitting zero pages.
void Init();

bool Contains(VAddr addr, u64 size);

// Maps fresh zeroed memory over [addr, addr+size) inside a reserved range.
bool Map(VAddr addr, u64 size, Prot prot);
// Returns [addr, addr+size) to the reserved, inaccessible state.
void Unmap(VAddr addr, u64 size);

} // namespace Core::AddressSpace
