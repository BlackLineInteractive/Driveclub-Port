// Driveclub Native: stand-in for shadPS4's core/memory.h.
//
// shadPS4's MemoryManager owns the guest address space; ours is
// src/core/memory.cpp. The renderer only needs a handful of queries and
// backing-memory accessors, implemented on top of it (memory.cpp, via
// dc_host.h).

#pragma once

#include <boost/icl/interval_set.hpp>

#include "common/enum.h"
#include "common/singleton.h"
#include "common/types.h"

namespace Vulkan {
class Rasterizer;
}

namespace Core {

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

class AddressSpace {
public:
    void Protect(VAddr virtual_addr, size_t size, MemoryPermission perms);

    // Only used by the Linux userfaultfd tracker, which we keep disabled.
    boost::icl::interval_set<VAddr> GetUsableRegions() const { return {}; }
};

class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_) { rasterizer = rasterizer_; }
    Vulkan::Rasterizer* GetRasterizer() const { return rasterizer; }

    AddressSpace& GetAddressSpace() { return impl; }

    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        // The PS4's GPU can only handle 40 bit addresses.
        const VAddr max_gpu_address{0x10000000000};
        return virtual_addr + size < max_gpu_address;
    }

    // Shrinks [addr, addr + size) to the mapped memory starting at addr.
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);

    // Reads guest memory, zero-filling unmapped holes.
    void CopySparseMemory(VAddr source, u8* dest, u64 size);

    // Writes guest memory without tripping write watches. False if the range
    // is not backed by direct memory.
    bool TryWriteBacking(void* address, const void* data, u64 size);

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};

using Memory = Common::Singleton<MemoryManager>;

} // namespace Core
