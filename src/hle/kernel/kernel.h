#pragma once

// Shared helpers for the libkernel HLE.

#include "common/types.h"

namespace Hle::Kernel {

constexpr s32 ORBIS_OK = 0;

// sce* functions return 0x8002xxxx with the errno in the low bits. FreeBSD and
// macOS share errno numbering for everything we use, so host errnos pass
// through unchanged.
constexpr s32 OrbisError(int err) {
    return err == 0 ? ORBIS_OK : static_cast<s32>(0x80020000u | static_cast<u32>(err));
}

// Guest timespec / timeval are FreeBSD amd64 layouts: same as macOS x86_64.

void RegisterPthread();

} // namespace Hle::Kernel
