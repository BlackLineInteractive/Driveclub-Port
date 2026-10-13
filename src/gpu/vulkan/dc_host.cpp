// DcHost (bridge/dc_host.h) on top of our memory manager and signal handler.

#include "dc_host.h"

#include <sys/mman.h>

#include "core/memory.h"
#include "core/signals.h"

namespace DcHost {

bool Query(std::uint64_t va, Mapping* out) {
    Core::Memory::Vma vma;
    if (!Core::Memory::Query(va, false, &vma) || vma.type == Core::Memory::VmaType::Free ||
        vma.type == Core::Memory::VmaType::Reserved) {
        return false;
    }
    out->start = vma.start;
    out->size = vma.size;
    out->direct = vma.type == Core::Memory::VmaType::Direct;
    out->phys = vma.phys;
    return true;
}

std::uint8_t* DirectView() {
    return static_cast<std::uint8_t*>(Core::Memory::DirectHostView());
}

std::uint64_t DirectSize() {
    return Core::Memory::kDirectSize;
}

std::vector<DirectRange> DirectRanges() {
    std::vector<DirectRange> out;
    for (const auto& r : Core::Memory::DirectMappings()) {
        out.push_back({r.start, r.size, r.phys});
    }
    return out;
}

std::uint64_t DirectGeneration() {
    return Core::Memory::DirectMapGeneration();
}

void Protect(std::uint64_t va, std::uint64_t size, bool read, bool write) {
    // Only the host protection changes: the guest-visible one stays recorded
    // in the memory manager and comes back when the watch is lifted.
    const int prot = (read ? PROT_READ : 0) | (write ? PROT_WRITE : 0);
    mprotect(reinterpret_cast<void*>(va), size, prot);
}

void SetFaultHandler(FaultHandler handler) {
    Core::Signals::SetFaultHandler(handler);
}

} // namespace DcHost
