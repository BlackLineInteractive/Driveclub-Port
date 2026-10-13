// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/save_data/save_data.h"

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::SaveData {

namespace {

struct OrbisSaveDataMountResult {
    char mount_point[16];
    u64 required_blocks;
    u32 unused;
    u32 mount_status;
    u8 reserved[32];
};

s32 sceSaveDataInitialize(void* param) {
    DC_INFO("sceSaveDataInitialize()");
    return 0;
}

s32 sceSaveDataTerminate() {
    DC_INFO("sceSaveDataTerminate()");
    return 0;
}

s32 sceSaveDataMount(const void* mount, OrbisSaveDataMountResult* mount_result) {
    if (mount_result) {
        std::memset(mount_result, 0, sizeof(*mount_result));
        std::strncpy(mount_result->mount_point, "/savedata0", sizeof(mount_result->mount_point) - 1);
    }
    DC_INFO("sceSaveDataMount() -> mount_point: /savedata0");
    return 0;
}

s32 sceSaveDataMount2(const void* mount, OrbisSaveDataMountResult* mount_result) {
    return sceSaveDataMount(mount, mount_result);
}

s32 sceSaveDataUmount(const void* mountPoint) {
    DC_INFO("sceSaveDataUmount()");
    return 0;
}

s32 sceSaveDataDirNameSearch(const void* cond, void* result) {
    if (result) {
        *reinterpret_cast<u32*>(result) = 0;
    }
    return 0;
}

s32 sceSaveDataSetParam(const void* mountPoint, u32 paramType, const void* param, size_t size) {
    return 0;
}

s32 sceSaveDataGetMountInfo(const void* mountPoint, void* info) {
    return 0;
}

s32 sceSaveDataDelete(const void* del) {
    return 0;
}

} // namespace

void RegisterSaveData() {
    constexpr const char* lib = "libSceSaveData";
    DC_HLE(lib, sceSaveDataInitialize);
    DC_HLE(lib, sceSaveDataTerminate);
    DC_HLE(lib, sceSaveDataMount);
    DC_HLE(lib, sceSaveDataMount2);
    DC_HLE(lib, sceSaveDataUmount);
    DC_HLE(lib, sceSaveDataDirNameSearch);
    DC_HLE(lib, sceSaveDataSetParam);
    DC_HLE(lib, sceSaveDataGetMountInfo);
    DC_HLE(lib, sceSaveDataDelete);
}

} // namespace Hle::SaveData
