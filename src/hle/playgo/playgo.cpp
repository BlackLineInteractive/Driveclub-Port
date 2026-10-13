// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/playgo/playgo.h"

#include <cstring>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::PlayGo {

namespace {

constexpr OrbisPlayGoHandle kValidHandle = 1;

s32 scePlayGoInitialize(const OrbisPlayGoInitParams* param) {
    DC_INFO("scePlayGoInitialize(bufSize=%u)", param ? param->bufSize : 0);
    return 0;
}

s32 scePlayGoTerminate() {
    DC_INFO("scePlayGoTerminate()");
    return 0;
}

s32 scePlayGoOpen(OrbisPlayGoHandle* outHandle, const void* param) {
    if (!outHandle) {
        return -1;
    }
    *outHandle = kValidHandle;
    DC_INFO("scePlayGoOpen() -> handle %u", kValidHandle);
    return 0;
}

s32 scePlayGoClose(OrbisPlayGoHandle handle) {
    DC_INFO("scePlayGoClose(handle=%u)", handle);
    return 0;
}

s32 scePlayGoGetLocus(OrbisPlayGoHandle handle, const OrbisPlayGoChunkId* chunkIds,
                       u32 count, OrbisPlayGoLocus* outLoci) {
    if (!outLoci) {
        return -1;
    }
    for (u32 i = 0; i < count; ++i) {
        // All chunks are extracted and present locally on disk
        outLoci[i] = OrbisPlayGoLocus::LocalFast;
    }
    DC_INFO("scePlayGoGetLocus(count=%u) -> all LocalFast", count);
    return 0;
}

s32 scePlayGoGetInstallSpeed(OrbisPlayGoHandle handle, OrbisPlayGoInstallSpeed* outSpeed) {
    if (outSpeed) {
        *outSpeed = OrbisPlayGoInstallSpeed::Full;
    }
    return 0;
}

s32 scePlayGoSetInstallSpeed(OrbisPlayGoHandle handle, OrbisPlayGoInstallSpeed speed) {
    DC_INFO("scePlayGoSetInstallSpeed(speed=%d)", static_cast<s32>(speed));
    return 0;
}

s32 scePlayGoGetToDoList(OrbisPlayGoHandle handle, OrbisPlayGoToDo* outTodoList,
                         u32 count, u32* outEntries) {
    if (outEntries) {
        *outEntries = 0; // Everything is already installed
    }
    return 0;
}

s32 scePlayGoSetToDoList(OrbisPlayGoHandle handle, const OrbisPlayGoToDo* todoList, u32 count) {
    return 0;
}

s32 scePlayGoGetEta(OrbisPlayGoHandle handle, const OrbisPlayGoChunkId* chunkIds,
                    u32 count, OrbisPlayGoEta* outEta) {
    if (outEta) {
        *outEta = 0; // 0 seconds remaining
    }
    return 0;
}

s32 scePlayGoPrefetch(OrbisPlayGoHandle handle, const OrbisPlayGoChunkId* chunkIds,
                      u32 count, OrbisPlayGoLocus minimumLocus) {
    return 0;
}

s32 scePlayGoGetChunkId(OrbisPlayGoHandle handle, OrbisPlayGoChunkId* outChunkIdList,
                        u32 count, u32* outEntries) {
    if (outEntries) {
        *outEntries = count;
    }
    if (outChunkIdList) {
        for (u32 i = 0; i < count; ++i) {
            outChunkIdList[i] = static_cast<OrbisPlayGoChunkId>(i);
        }
    }
    return 0;
}

s32 scePlayGoGetProgress(OrbisPlayGoHandle handle, const OrbisPlayGoChunkId* chunkIds,
                         u32 count, OrbisPlayGoProgress* outProgress) {
    if (outProgress) {
        outProgress->progressSize = 1000000000ULL;
        outProgress->totalSize = 1000000000ULL;
    }
    return 0;
}

s32 scePlayGoGetLanguageMask(OrbisPlayGoHandle handle, OrbisPlayGoLanguageMask* outLanguageMask) {
    if (outLanguageMask) {
        *outLanguageMask = 1ULL << 63;
    }
    return 0;
}

s32 scePlayGoSetLanguageMask(OrbisPlayGoHandle handle, OrbisPlayGoLanguageMask languageMask) {
    return 0;
}

} // namespace

void RegisterPlayGo() {
    constexpr const char* lib = "libScePlayGo";
    DC_HLE(lib, scePlayGoInitialize);
    DC_HLE(lib, scePlayGoTerminate);
    DC_HLE(lib, scePlayGoOpen);
    DC_HLE(lib, scePlayGoClose);
    DC_HLE(lib, scePlayGoGetLocus);
    DC_HLE(lib, scePlayGoGetInstallSpeed);
    DC_HLE(lib, scePlayGoSetInstallSpeed);
    DC_HLE(lib, scePlayGoGetToDoList);
    DC_HLE(lib, scePlayGoSetToDoList);
    DC_HLE(lib, scePlayGoGetEta);
    DC_HLE(lib, scePlayGoPrefetch);
    DC_HLE(lib, scePlayGoGetChunkId);
    DC_HLE(lib, scePlayGoGetProgress);
    DC_HLE(lib, scePlayGoGetLanguageMask);
    DC_HLE(lib, scePlayGoSetLanguageMask);
}

} // namespace Hle::PlayGo
