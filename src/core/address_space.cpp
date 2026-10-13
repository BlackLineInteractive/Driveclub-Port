#include "core/address_space.h"

#include <sys/mman.h>

#include "common/log.h"

// Zero-fill segments placed at fixed addresses by -segaddr (CMakeLists.txt).
// Sizes are kMax - kMin of the matching range.
asm(".zerofill GUEST_SYSMGD,GUEST_SYSMGD,__guest_sysmgd,0x7FFBFC000");
asm(".zerofill GUEST_SYSRSV,GUEST_SYSRSV,__guest_sysrsv,0x7C0004000");
asm(".zerofill GUEST_USER,GUEST_USER,__guest_user,0x5F9000000000");

namespace Core::AddressSpace {

namespace {

struct Range {
    VAddr min, max;
};

constexpr Range kRanges[] = {
    {kSystemManagedMin, kSystemManagedMax},
    {kSystemReservedMin, kSystemReservedMax},
    {kUserMin, kUserMax},
};

int ToHost(Prot prot) {
    const int p = static_cast<int>(prot);
    int host = PROT_NONE;
    host |= (p & static_cast<int>(Prot::Read)) ? PROT_READ : 0;
    host |= (p & static_cast<int>(Prot::Write)) ? PROT_WRITE : 0;
    host |= (p & static_cast<int>(Prot::Exec)) ? PROT_EXEC | PROT_READ : 0;
    return host;
}

void* Reserve(VAddr addr, u64 size) {
    return mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
                MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
}

} // namespace

void Init() {
    for (const Range& r : kRanges) {
        if (Reserve(r.min, r.max - r.min) == MAP_FAILED) {
            DC_ERROR("cannot reserve guest range %#llx-%#llx", r.min, r.max);
        }
    }
}

bool Contains(VAddr addr, u64 size) {
    for (const Range& r : kRanges) {
        if (addr >= r.min && addr + size <= r.max && addr + size >= addr) {
            return true;
        }
    }
    return false;
}

bool Map(VAddr addr, u64 size, Prot prot) {
    if (!Contains(addr, size)) {
        DC_ERROR("guest map %#llx+%#llx is outside the reserved ranges", addr, size);
        return false;
    }
    void* p = mmap(reinterpret_cast<void*>(addr), size, ToHost(prot),
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return p != MAP_FAILED;
}

void Unmap(VAddr addr, u64 size) {
    if (Contains(addr, size)) {
        Reserve(addr, size);
    }
}

} // namespace Core::AddressSpace
