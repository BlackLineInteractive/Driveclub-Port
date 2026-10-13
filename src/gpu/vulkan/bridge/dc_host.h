#pragma once

// What the vendored shadPS4 GPU code needs from the rest of Driveclub Native.
//
// The shadPS4 sources and ours both have "common/..." and "core/..." headers,
// so they are built with separate include roots and only talk through plain
// C++ declarations like these. Implemented in src/gpu/vulkan/dc_host.cpp.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DcHost {

// ---- guest memory (guest virtual addresses are host addresses) ----

// The mapping containing `va`: its bounds, and for direct memory the physical
// address of `start`.
struct Mapping {
    std::uint64_t start = 0;
    std::uint64_t size = 0;
    bool direct = false;
    std::uint64_t phys = 0;
};
bool Query(std::uint64_t va, Mapping* out);

// Direct memory mapped once more without guest protections: byte offset ==
// physical address. Writes through it never fault on write-watched pages.
std::uint8_t* DirectView();
std::uint64_t DirectSize();

struct DirectRange {
    std::uint64_t va;
    std::uint64_t size;
    std::uint64_t phys;
};
// Guest virtual -> physical direct mappings; `generation` changes with them.
std::vector<DirectRange> DirectRanges();
std::uint64_t DirectGeneration();

// Changes host page protection of a guest range (write watching).
void Protect(std::uint64_t va, std::uint64_t size, bool read, bool write);

// ---- signals ----

// Runs first on SIGSEGV/SIGBUS; returns true if it resolved the fault.
using FaultHandler = bool (*)(void* ucontext, void* fault_address);
void SetFaultHandler(FaultHandler handler);

} // namespace DcHost
