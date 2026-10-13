// libkernel memory: direct/flexible memory and the POSIX mmap family.

#include <cerrno>
#include <cstring>

#include "common/log.h"
#include "core/memory.h"
#include "hle/hle.h"
#include "hle/kernel/kernel.h"

namespace Hle::Kernel {

namespace {

namespace Mem = Core::Memory;

s32 sceKernelAllocateDirectMemory(s64 search_start, s64 search_end, u64 len, u64 align,
                                  int memory_type, s64* phys_out) {
    u64 phys = 0;
    const int err = Mem::AllocateDirect(search_start, search_end, len, align, memory_type, &phys);
    if (err == 0) {
        *phys_out = static_cast<s64>(phys);
    }
    return OrbisError(err);
}

s32 sceKernelReleaseDirectMemory(s64 start, u64 len) {
    return OrbisError(Mem::ReleaseDirect(start, len));
}

u64 sceKernelGetDirectMemorySize() {
    return Mem::kDirectSize;
}

s32 sceKernelMapNamedDirectMemory(void** addr, u64 len, int prot, int flags, s64 phys,
                                  u64 align, const char* name) {
    VAddr va = reinterpret_cast<VAddr>(*addr);
    const int err = Mem::MapDirect(&va, len, prot, flags, phys, align, name);
    if (err == 0) {
        *addr = reinterpret_cast<void*>(va);
    }
    return OrbisError(err);
}

s32 sceKernelMapDirectMemory(void** addr, u64 len, int prot, int flags, s64 phys, u64 align) {
    return sceKernelMapNamedDirectMemory(addr, len, prot, flags, phys, align, nullptr);
}

s32 sceKernelMapNamedFlexibleMemory(void** addr, u64 len, int prot, int flags, const char* name) {
    VAddr va = reinterpret_cast<VAddr>(*addr);
    const int err = Mem::MapFlexible(&va, len, prot, flags, name);
    if (err == 0) {
        *addr = reinterpret_cast<void*>(va);
    }
    return OrbisError(err);
}

s32 sceKernelMapFlexibleMemory(void** addr, u64 len, int prot, int flags) {
    return sceKernelMapNamedFlexibleMemory(addr, len, prot, flags, nullptr);
}

s32 sceKernelReserveVirtualRange(void** addr, u64 len, int flags, u64 align) {
    VAddr va = reinterpret_cast<VAddr>(*addr);
    const int err = Mem::Reserve(&va, len, flags, align);
    if (err == 0) {
        *addr = reinterpret_cast<void*>(va);
    }
    return OrbisError(err);
}

s32 sceKernelMunmap(void* addr, u64 len) {
    return OrbisError(Mem::Unmap(reinterpret_cast<VAddr>(addr), len));
}

s32 sceKernelMprotect(void* addr, u64 len, int prot) {
    return OrbisError(Mem::Protect(reinterpret_cast<VAddr>(addr), len, prot));
}

s32 sceKernelMlock(const void*, u64) {
    return ORBIS_OK;
}

s32 sceKernelSetVirtualRangeName(void* addr, u64 len, const char* name) {
    Mem::SetName(reinterpret_cast<VAddr>(addr), len, name);
    return ORBIS_OK;
}

s32 sceKernelSetPrtAperture(int, void*, u64) {
    return ORBIS_OK;
}

struct VirtualQueryInfo {
    u64 start;
    u64 end;
    u64 offset;
    s32 protection;
    s32 memory_type;
    u8 is_flexible : 1;
    u8 is_direct : 1;
    u8 is_stack : 1;
    u8 is_pooled : 1;
    u8 is_committed : 1;
    char name[32];
};
static_assert(sizeof(VirtualQueryInfo) == 72);

s32 sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, u64 info_size) {
    Mem::Vma v;
    if (!Mem::Query(reinterpret_cast<VAddr>(addr), flags & 1, &v)) {
        return OrbisError(EACCES);
    }
    std::memset(info, 0, sizeof(*info));
    info->start = v.start;
    info->end = v.start + v.size;
    info->offset = v.phys;
    info->protection = v.prot;
    info->is_flexible = v.type == Mem::VmaType::Flexible;
    info->is_direct = v.type == Mem::VmaType::Direct;
    info->is_committed = v.type != Mem::VmaType::Reserved;
    std::strncpy(info->name, v.name.c_str(), sizeof(info->name) - 1);
    return ORBIS_OK;
}

// POSIX mmap with FreeBSD flag values.
constexpr int kGuestMapFixed = 0x10;
constexpr int kGuestMapAnon = 0x1000;

void* posix_mmap(void* addr, u64 len, int prot, int flags, int fd, s64 offset) {
    if (!(flags & kGuestMapAnon) || fd != -1) {
        DC_WARN("mmap of a file (fd %d) is not supported yet", fd);
        return reinterpret_cast<void*>(-1);
    }
    VAddr va = reinterpret_cast<VAddr>(addr);
    const int sce_flags = (flags & kGuestMapFixed) ? Mem::kMapFixed : 0;
    if (Mem::MapFlexible(&va, len, prot, sce_flags) != 0) {
        return reinterpret_cast<void*>(-1);
    }
    return reinterpret_cast<void*>(va);
}

int posix_munmap(void* addr, u64 len) {
    return Mem::Unmap(reinterpret_cast<VAddr>(addr), len) == 0 ? 0 : -1;
}

int posix_msync(void*, u64, int) {
    return 0;
}

int posix_getpagesize() {
    return 0x4000;
}

} // namespace

void RegisterMemory() {
    constexpr const char* k = "libkernel";
    DC_HLE(k, sceKernelAllocateDirectMemory);
    DC_HLE(k, sceKernelReleaseDirectMemory);
    DC_HLE(k, sceKernelGetDirectMemorySize);
    DC_HLE(k, sceKernelMapDirectMemory);
    DC_HLE(k, sceKernelMapNamedDirectMemory);
    DC_HLE(k, sceKernelMapFlexibleMemory);
    DC_HLE(k, sceKernelMapNamedFlexibleMemory);
    DC_HLE(k, sceKernelReserveVirtualRange);
    DC_HLE(k, sceKernelMunmap);
    DC_HLE(k, sceKernelMprotect);
    DC_HLE(k, sceKernelMlock);
    DC_HLE(k, sceKernelSetVirtualRangeName);
    DC_HLE(k, sceKernelSetPrtAperture);
    DC_HLE(k, sceKernelVirtualQuery);
    for (const char* lib : {"libkernel", "libScePosix"}) {
        DC_HLE_AS(lib, "mmap", posix_mmap);
        DC_HLE_AS(lib, "munmap", posix_munmap);
        DC_HLE_AS(lib, "msync", posix_msync);
        DC_HLE_AS(lib, "getpagesize", posix_getpagesize);
    }
}

} // namespace Hle::Kernel
