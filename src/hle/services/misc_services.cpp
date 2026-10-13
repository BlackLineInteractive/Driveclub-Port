// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/services/misc_services.h"

#include <algorithm>
#include <sys/time.h>
#include <unistd.h>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::MiscServices {

namespace {

// libSceCommonDialog
s32 sceCommonDialogInitialize() {
    return 0;
}

// libSceGameLiveStreaming
s32 sceGameLiveStreamingInitialize() {
    return 0;
}

s32 sceGameLiveStreamingEnableLiveStreaming() {
    return 0;
}

s32 sceGameLiveStreamingPermitLiveStreaming() {
    return 0;
}

// libSceVideoRecording
s32 sceVideoRecordingSetInfo(const void* info) {
    return 0;
}

// libSceScreenShot
s32 sceScreenShotEnable() {
    return 0;
}

// libSceUsbd
s32 sceUsbdInit(void* param) {
    return 0;
}

s32 sceUsbdHandleEventsTimeout(struct timeval* tv) {
    if (tv) {
        const u64 us = tv->tv_sec * 1000000ULL + tv->tv_usec;
        if (us > 0) {
            usleep(static_cast<useconds_t>(std::min(us, 10000ULL)));
        } else {
            usleep(1000);
        }
    } else {
        usleep(10000);
    }
    return 0;
}

} // namespace

void RegisterMiscServices() {
    DC_HLE("libSceCommonDialog", sceCommonDialogInitialize);
    DC_HLE("libSceGameLiveStreaming", sceGameLiveStreamingInitialize);
    DC_HLE("libSceGameLiveStreaming", sceGameLiveStreamingEnableLiveStreaming);
    DC_HLE("libSceGameLiveStreaming", sceGameLiveStreamingPermitLiveStreaming);
    DC_HLE("libSceVideoRecording", sceVideoRecordingSetInfo);
    DC_HLE("libSceScreenShot", sceScreenShotEnable);
    DC_HLE("libSceUsbd", sceUsbdInit);
    DC_HLE("libSceUsbd", sceUsbdHandleEventsTimeout);
}

} // namespace Hle::MiscServices
