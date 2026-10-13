#pragma once

#include "common/types.h"

namespace Hle::PlayGo {

using OrbisPlayGoHandle = u32;
using OrbisPlayGoChunkId = u16;
using OrbisPlayGoEta = s64;
using OrbisPlayGoLanguageMask = u64;

enum class OrbisPlayGoLocus : s8 {
    NotDownloaded = 0,
    LocalSlow = 2,
    LocalFast = 3,
};

enum class OrbisPlayGoInstallSpeed : s32 {
    Suspended = 0,
    Trickle = 1,
    Full = 2,
};

struct OrbisPlayGoInitParams {
    const void* bufAddr;
    u32 bufSize;
    u32 reserved;
};

struct OrbisPlayGoToDo {
    OrbisPlayGoChunkId chunkId;
    OrbisPlayGoLocus locus;
    s8 reserved;
};

struct OrbisPlayGoProgress {
    u64 progressSize;
    u64 totalSize;
};

void RegisterPlayGo();

} // namespace Hle::PlayGo
