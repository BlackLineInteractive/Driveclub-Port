// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/save_data/save_data.h"

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::SaveData {

namespace {

s32 sceSaveDataInitialize(void* param) {
    DC_INFO("sceSaveDataInitialize()");
    return 0;
}

s32 sceSaveDataTerminate() {
    DC_INFO("sceSaveDataTerminate()");
    return 0;
}

} // namespace

void RegisterSaveData() {
    constexpr const char* lib = "libSceSaveData";
    DC_HLE(lib, sceSaveDataInitialize);
    DC_HLE(lib, sceSaveDataTerminate);
}

} // namespace Hle::SaveData
