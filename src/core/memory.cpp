#include "core/memory.h"

#include <cerrno>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/log.h"
#include "core/address_space.h"

namespace Core::Memory {

namespace {

constexpr u64 kPage = 0x4000;
// Where mappings without an address hint go: above the executable and game
// modules, inside the reserved "system managed" range.
constexpr VAddr kDefaultSearchBase = 0x200000000;

std::mutex g_lock;
int g_direct_fd = -1;
std::map<u64, u64> g_direct_used; // phys start -> size
std::map<VAddr, Vma> g_vmas;      // start -> mapping (non-overlapping)

int HostProt(int prot) {
    int host = PROT_NONE;
    if (prot & kProtCpuRead) {
        host |= PROT_READ;
    }
    if (prot & kProtCpuWrite) {
        host |= PROT_READ | PROT_WRITE;
    }
    if (prot & kProtCpuExec) {
        host |= PROT_READ | PROT_EXEC;
    }
    // We emulate the GPU on the CPU side, so GPU-visible memory must stay
    // CPU-accessible to us.
    if (prot & (kProtGpuRead | kProtGpuWrite)) {
        host |= PROT_READ | PROT_WRITE;
    }
    return host;
}

bool Overlaps(VAddr start, u64 size) {
    auto it = g_vmas.upper_bound(start);
    if (it != g_vmas.begin()) {
        auto prev = std::prev(it);
        if (prev->second.start + prev->second.size > start) {
            return true;
        }
    }
    return it != g_vmas.end() && it->first < start + size;
}

// First free, aligned gap of `size` bytes at or above `hint`.
VAddr FindFree(VAddr hint, u64 size, u64 align) {
    VAddr addr = AlignUp(std::max(hint, kDefaultSearchBase), std::max(align, kPage));
    for (;;) {
        if (!AddressSpace::Contains(addr, size)) {
            return 0;
        }
        auto it = g_vmas.upper_bound(addr);
        if (it != g_vmas.begin()) {
            auto prev = std::prev(it);
            const VAddr prev_end = prev->second.start + prev->second.size;
            if (prev_end > addr) {
                addr = AlignUp(prev_end, std::max(align, kPage));
                continue;
            }
        }
        if (it != g_vmas.end() && it->first < addr + size) {
            addr = AlignUp(it->first + it->second.size, std::max(align, kPage));
            continue;
        }
        return addr;
    }
}

// Removes [start, start+size) from the VMA map, splitting partial overlaps.
void Carve(VAddr start, u64 size) {
    const VAddr end = start + size;
    auto it = g_vmas.upper_bound(start);
    if (it != g_vmas.begin()) {
        --it;
    }
    while (it != g_vmas.end() && it->first < end) {
        Vma v = it->second;
        const VAddr v_end = v.start + v.size;
        if (v_end <= start) {
            ++it;
            continue;
        }
        it = g_vmas.erase(it);
        if (v.start < start) {
            Vma head = v;
            head.size = start - v.start;
            g_vmas.emplace(head.start, head);
        }
        if (v_end > end) {
            Vma tail = v;
            tail.start = end;
            tail.size = v_end - end;
            if (tail.type == VmaType::Direct) {
                tail.phys += end - v.start;
            }
            g_vmas.emplace(tail.start, tail);
        }
    }
}

// Picks the final address for a new mapping; returns errno or 0.
int Place(VAddr* addr, u64 size, int flags, u64 align) {
    if (flags & kMapFixed) {
        if (*addr % kPage != 0 || !AddressSpace::Contains(*addr, size)) {
            return EINVAL;
        }
        if (Overlaps(*addr, size)) {
            if (flags & kMapNoOverwrite) {
                return ENOMEM;
            }
            Carve(*addr, size);
        }
        return 0;
    }
    const VAddr found = FindFree(*addr, size, align);
    if (!found) {
        return ENOMEM;
    }
    *addr = found;
    return 0;
}

} // namespace

void Init() {
    char name[64];
    std::snprintf(name, sizeof(name), "/dc_direct_%d", getpid());
    g_direct_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (g_direct_fd < 0) {
        DC_ERROR("shm_open failed: %d", errno);
        return;
    }
    shm_unlink(name); // the fd keeps it alive; nothing leaks if we crash
    if (ftruncate(g_direct_fd, static_cast<off_t>(kDirectSize)) != 0) {
        DC_ERROR("cannot size direct memory to %llu MB: %d", kDirectSize >> 20, errno);
    }
}

u64 DirectMemoryAvailable() {
    std::scoped_lock lk(g_lock);
    u64 used = 0;
    for (const auto& [start, size] : g_direct_used) {
        used += size;
    }
    return kDirectSize - used;
}

int AllocateDirect(u64 search_start, u64 search_end, u64 size, u64 align, int, u64* phys_out) {
    std::scoped_lock lk(g_lock);
    align = std::max(align, kPage);
    size = AlignUp(size, kPage);
    search_end = std::min(search_end == 0 ? kDirectSize : search_end, kDirectSize);

    u64 candidate = AlignUp(search_start, align);
    for (const auto& [start, used] : g_direct_used) {
        if (start + used <= candidate) {
            continue;
        }
        if (start >= candidate + size) {
            break;
        }
        candidate = AlignUp(start + used, align);
    }
    if (candidate + size > search_end) {
        return EAGAIN;
    }
    g_direct_used.emplace(candidate, size);
    *phys_out = candidate;
    return 0;
}

int ReleaseDirect(u64 phys, u64 size) {
    std::scoped_lock lk(g_lock);
    const u64 end = phys + AlignUp(size, kPage);
    for (auto it = g_direct_used.begin(); it != g_direct_used.end();) {
        const u64 s = it->first, e = it->first + it->second;
        if (e <= phys || s >= end) {
            ++it;
            continue;
        }
        it = g_direct_used.erase(it);
        if (s < phys) {
            g_direct_used.emplace(s, phys - s);
        }
        if (e > end) {
            g_direct_used.emplace(end, e - end);
        }
    }
    return 0;
}

int MapDirect(VAddr* addr, u64 size, int prot, int flags, u64 phys, u64 align, const char* name) {
    std::scoped_lock lk(g_lock);
    size = AlignUp(size, kPage);
    if (phys + size > kDirectSize) {
        return EINVAL;
    }
    if (const int err = Place(addr, size, flags, align)) {
        return err;
    }
    void* p = mmap(reinterpret_cast<void*>(*addr), size, HostProt(prot), MAP_SHARED | MAP_FIXED,
                   g_direct_fd, static_cast<off_t>(phys));
    if (p == MAP_FAILED) {
        DC_ERROR("direct map %#llx+%#llx failed: %d", *addr, size, errno);
        return ENOMEM;
    }
    g_vmas[*addr] = {*addr, size, prot, VmaType::Direct, phys, name ? name : ""};
    return 0;
}

int MapFlexible(VAddr* addr, u64 size, int prot, int flags, const char* name) {
    std::scoped_lock lk(g_lock);
    size = AlignUp(size, kPage);
    if (const int err = Place(addr, size, flags, kPage)) {
        return err;
    }
    void* p = mmap(reinterpret_cast<void*>(*addr), size, HostProt(prot),
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        return ENOMEM;
    }
    g_vmas[*addr] = {*addr, size, prot, VmaType::Flexible, 0, name ? name : ""};
    return 0;
}

int Reserve(VAddr* addr, u64 size, int flags, u64 align) {
    std::scoped_lock lk(g_lock);
    size = AlignUp(size, kPage);
    if (const int err = Place(addr, size, flags, align)) {
        return err;
    }
    AddressSpace::Unmap(*addr, size);
    g_vmas[*addr] = {*addr, size, 0, VmaType::Reserved, 0, ""};
    return 0;
}

int Unmap(VAddr addr, u64 size) {
    std::scoped_lock lk(g_lock);
    size = AlignUp(size, kPage);
    Carve(addr, size);
    AddressSpace::Unmap(addr, size);
    return 0;
}

int Protect(VAddr addr, u64 size, int prot) {
    std::scoped_lock lk(g_lock);
    size = AlignUp(size, kPage);
    if (mprotect(reinterpret_cast<void*>(addr), size, HostProt(prot)) != 0) {
        return errno;
    }
    for (auto& [start, v] : g_vmas) {
        if (start >= addr && start + v.size <= addr + size) {
            v.prot = prot;
        }
    }
    return 0;
}

void SetName(VAddr addr, u64 size, const char* name) {
    std::scoped_lock lk(g_lock);
    for (auto& [start, v] : g_vmas) {
        if (start >= addr && start < addr + size) {
            v.name = name ? name : "";
        }
    }
}

bool Query(VAddr addr, bool find_next, Vma* out) {
    std::scoped_lock lk(g_lock);
    auto it = g_vmas.upper_bound(addr);
    if (it != g_vmas.begin()) {
        auto prev = std::prev(it);
        if (prev->second.start + prev->second.size > addr) {
            *out = prev->second;
            return true;
        }
    }
    if (find_next && it != g_vmas.end()) {
        *out = it->second;
        return true;
    }
    return false;
}

} // namespace Core::Memory
