// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/pad/pad.h"

#include <cstring>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Pad {

namespace {

constexpr s32 kPadHandle = 1;
constexpr s32 kErrorInvalidPortType = -2137980926; // 0x80920002

s32 scePadInit() {
    DC_INFO("scePadInit()");
    return 0;
}

s32 scePadOpen(s32 userId, s32 type, s32 index, const void* param) {
    if (type != 0) {
        return kErrorInvalidPortType;
    }
    DC_INFO("scePadOpen(userId=%#x, type=%d, index=%d) -> handle %d", userId, type, index, kPadHandle);
    return kPadHandle;
}

s32 scePadOpenExt(s32 userId, s32 type, s32 index, const void* param) {
    if (type != 0) {
        return kErrorInvalidPortType;
    }
    DC_INFO("scePadOpenExt(userId=%#x, type=%d, index=%d) -> handle %d", userId, type, index, kPadHandle);
    return kPadHandle;
}

s32 scePadClose(s32 handle) {
    DC_INFO("scePadClose(handle=%d)", handle);
    return 0;
}

s32 scePadReadState(s32 handle, OrbisPadData* data) {
    if (data) {
        std::memset(data, 0, sizeof(*data));
        data->leftStick = {128, 128};
        data->rightStick = {128, 128};
        data->orientation[3] = 1.0f;
        data->acceleration[2] = 9.8f;
        data->connected = true;
        data->connectedCount = 1;
    }
    return 0;
}

s32 scePadRead(s32 handle, OrbisPadData* data, s32 count) {
    if (data && count > 0) {
        for (s32 i = 0; i < count; ++i) {
            std::memset(&data[i], 0, sizeof(OrbisPadData));
            data[i].leftStick = {128, 128};
            data[i].rightStick = {128, 128};
            data[i].orientation[3] = 1.0f;
            data[i].acceleration[2] = 9.8f;
            data[i].connected = true;
            data[i].connectedCount = 1;
        }
    }
    return count;
}

s32 scePadGetControllerInformation(s32 handle, OrbisPadControllerInformation* info) {
    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->touchPadInfo.pixelDensity = 1.0f;
        info->touchPadInfo.resX = 1920;
        info->touchPadInfo.resY = 942;
        info->connected = true;
        info->connectedCount = 1;
    }
    return 0;
}

s32 scePadGetExtControllerInformation(s32 handle, void* pInfo) {
    return scePadGetControllerInformation(handle, reinterpret_cast<OrbisPadControllerInformation*>(pInfo));
}

s32 scePadSetTiltCorrectionState(s32 handle, bool bEnable) {
    return 0;
}

s32 scePadSetVibration(s32 handle, const void* param) {
    return 0;
}

s32 scePadResetOrientation(s32 handle) {
    return 0;
}

} // namespace

void RegisterPad() {
    constexpr const char* lib = "libScePad";
    DC_HLE(lib, scePadInit);
    DC_HLE(lib, scePadOpen);
    DC_HLE(lib, scePadOpenExt);
    DC_HLE(lib, scePadClose);
    DC_HLE(lib, scePadReadState);
    DC_HLE(lib, scePadRead);
    DC_HLE(lib, scePadGetControllerInformation);
    DC_HLE(lib, scePadGetExtControllerInformation);
    DC_HLE(lib, scePadSetTiltCorrectionState);
    DC_HLE(lib, scePadSetVibration);
    DC_HLE(lib, scePadResetOrientation);
}

} // namespace Hle::Pad
