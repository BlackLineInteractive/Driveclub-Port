#pragma once

// Guest memory manager: PS4 direct ("physical") memory, flexible memory and
// the virtual mappings of both.
//
// Direct memory is backed by one shared-memory object, so mapping the same
// physical range at several virtual addresses aliases the same bytes, as on
// the console (games map GPU buffers twice with different attributes).

#include <string>

#include "common/types.h"

namespace Core::Memory {

constexpr u64 kTotalMemory = 5248ull << 20;
constexpr u64 kFlexibleSize = 512ull << 20;
constexpr u64 kDirectSize = kTotalMemory - kFlexibleSize;

// PS4 protection bits.
constexpr int kProtCpuRead = 0x1;
constexpr int kProtCpuWrite = 0x2;
constexpr int kProtCpuExec = 0x4;
constexpr int kProtGpuRead = 0x10;
constexpr int kProtGpuWrite = 0x20;

// PS4 mapping flags.
constexpr int kMapFixed = 0x10;
constexpr int kMapNoOverwrite = 0x80;

enum class VmaType { Free, Direct, Flexible, Reserved };

struct Vma {
    VAddr start = 0;
    u64 size = 0;
    int prot = 0;
    VmaType type = VmaType::Free;
    u64 phys = 0; // Direct only
    std::string name;
};

void Init();

// Records a range the loader owns (module images, patch areas) so guest
// mappings never land on it.
void MarkUsed(VAddr start, u64 size, const char* name);

u64 DirectMemoryAvailable();

// Direct (physical) memory. Errors are errno values.
int AllocateDirect(u64 search_start, u64 search_end, u64 size, u64 align, int memory_type,
                   u64* phys_out);
int ReleaseDirect(u64 phys, u64 size);

// Virtual mappings. On input *addr is a hint (or the exact address with
// kMapFixed); on output it is where the mapping landed.
int MapDirect(VAddr* addr, u64 size, int prot, int flags, u64 phys, u64 align,
              const char* name = nullptr);
int MapFlexible(VAddr* addr, u64 size, int prot, int flags, const char* name = nullptr);
int Reserve(VAddr* addr, u64 size, int flags, u64 align);
int Unmap(VAddr addr, u64 size);
int Protect(VAddr addr, u64 size, int prot);
void SetName(VAddr addr, u64 size, const char* name);

// The mapping containing `addr`, or with `find_next` the first one above it.
bool Query(VAddr addr, bool find_next, Vma* out);

} // namespace Core::Memory
