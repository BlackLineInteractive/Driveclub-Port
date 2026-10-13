#pragma once

// Guest thread-local storage, FreeBSD/PS4 "variant II" layout:
//
//   [ static TLS of the executable ][ Tcb ]
//                                    ^ fs:0 reads Tcb::self
//
// Other modules' TLS blocks are allocated lazily through the DTV and reached
// via __tls_get_addr. Guest code finds the TCB with `mov reg, fs:[0]`; on
// macOS we point FS at a small LDT segment holding a copy of the TCB, since
// the FS base itself cannot be set from user space.

#include "common/types.h"

namespace Core::Tls {

struct Tcb {
    Tcb* self;
    void** dtv; // dtv[0] = generation, dtv[1] = entry count, dtv[1 + index] = block
    void* thread;
    void* spare[2];
    u64 canary;
    void* fiber;
};

struct ModuleTls {
    VAddr image;
    u64 filesz;
    u64 memsz;
    u64 align;
};

// Registers a module's TLS template under its 1-based TLS index. Index 1 (the
// executable) becomes the static block in front of each TCB.
void RegisterModule(u32 index, const ModuleTls& tls);

// Allocates TLS for the calling thread and points FS at it.
Tcb* SetupThread();
// Frees the calling thread's TLS.
void TeardownThread();

Tcb* Current();

// Backs the guest's __tls_get_addr.
struct TlsIndex {
    u64 module;
    u64 offset;
};
void* GetAddr(const TlsIndex* index);

} // namespace Core::Tls
