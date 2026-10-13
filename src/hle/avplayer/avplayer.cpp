// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/avplayer/avplayer.h"

#include <cstdint>
#include <cstring>

#include "common/log.h"
#include "common/types.h"
#include "hle/hle.h"

namespace Hle::AvPlayer {

namespace {

struct PlayerContext {
    uint32_t handle = 1;
    bool active = false;
    uint64_t current_time_ms = 0;
};

static PlayerContext g_player;

void* sceAvPlayerInit(void* init_data) {
    (void)init_data;
    DC_INFO("sceAvPlayerInit()");
    g_player.active = false;
    g_player.current_time_ms = 0;
    return &g_player;
}

s32 sceAvPlayerInitEx(const void* p_data, void** p_player) {
    (void)p_data;
    DC_INFO("sceAvPlayerInitEx()");
    g_player.active = false;
    g_player.current_time_ms = 0;
    if (p_player) {
        *p_player = &g_player;
    }
    return 0;
}

s32 sceAvPlayerClose(void* handle) {
    DC_INFO("sceAvPlayerClose(handle=%p)", handle);
    if (handle == &g_player) {
        g_player.active = false;
    }
    return 0;
}

s32 sceAvPlayerAddSource(void* handle, const char* filename) {
    DC_INFO("sceAvPlayerAddSource(handle=%p, file=%s)", handle, filename ? filename : "null");
    return 0;
}

s32 sceAvPlayerAddSourceEx(void* handle, u32 uri_type, void* source_details) {
    (void)handle;
    (void)uri_type;
    (void)source_details;
    DC_INFO("sceAvPlayerAddSourceEx()");
    return 0;
}

s32 sceAvPlayerStart(void* handle) {
    DC_INFO("sceAvPlayerStart(handle=%p)", handle);
    if (handle == &g_player) {
        g_player.active = false; // Immediately finish video to allow skipping to menu
    }
    return 0;
}

s32 sceAvPlayerStartEx(void* handle) {
    (void)handle;
    return sceAvPlayerStart(handle);
}

s32 sceAvPlayerStop(void* handle) {
    DC_INFO("sceAvPlayerStop(handle=%p)", handle);
    if (handle == &g_player) {
        g_player.active = false;
    }
    return 0;
}

s32 sceAvPlayerPause(void* handle) {
    (void)handle;
    return 0;
}

s32 sceAvPlayerResume(void* handle) {
    (void)handle;
    return 0;
}

bool sceAvPlayerIsActive(void* handle) {
    // Return false so game knows the video is finished
    return (handle == &g_player) ? g_player.active : false;
}

u64 sceAvPlayerCurrentTime(void* handle) {
    (void)handle;
    return 0;
}

bool sceAvPlayerGetAudioData(void* handle, void* p_info) {
    (void)handle;
    (void)p_info;
    return false; // EOF
}

bool sceAvPlayerGetVideoData(void* handle, void* p_info) {
    (void)handle;
    (void)p_info;
    return false; // EOF
}

bool sceAvPlayerGetVideoDataEx(void* handle, void* p_info) {
    (void)handle;
    (void)p_info;
    return false; // EOF
}

s32 sceAvPlayerGetStreamInfo(void* handle, u32 stream_id, void* p_info) {
    (void)handle;
    (void)stream_id;
    (void)p_info;
    return 0;
}

s32 sceAvPlayerGetStreamInfoEx(void* handle, u32 stream_id, void* p_info) {
    (void)handle;
    (void)stream_id;
    (void)p_info;
    return 0;
}

s32 sceAvPlayerEnableStream(void* handle, u32 stream_id) {
    (void)handle;
    (void)stream_id;
    return 0;
}

s32 sceAvPlayerDisableStream(void* handle, u32 stream_id) {
    (void)handle;
    (void)stream_id;
    return 0;
}

s32 sceAvPlayerChangeStream(void* handle) {
    (void)handle;
    return 0;
}

s32 sceAvPlayerSetLooping(void* handle, bool loop) {
    (void)handle;
    (void)loop;
    return 0;
}

s32 sceAvPlayerSetAvSyncMode(void* handle, u32 mode) {
    (void)handle;
    (void)mode;
    return 0;
}

s32 sceAvPlayerPostInit(void* handle, void* data) {
    (void)handle;
    (void)data;
    return 0;
}

s32 sceAvPlayerSetLogCallback(void* cb, void* user_data) {
    (void)cb;
    (void)user_data;
    return 0;
}

s32 sceAvPlayerSetTrickSpeed(void* handle, s32 speed) {
    (void)handle;
    (void)speed;
    return 0;
}

s32 sceAvPlayerStreamCount(void* handle) {
    (void)handle;
    return 0;
}

s32 sceAvPlayerJumpToTime(void* handle, u64 time) {
    (void)handle;
    (void)time;
    return 0;
}

} // namespace

void RegisterAvPlayer() {
    constexpr const char* lib = "libSceAvPlayer";
    DC_HLE(lib, sceAvPlayerInit);
    DC_HLE(lib, sceAvPlayerInitEx);
    DC_HLE(lib, sceAvPlayerClose);
    DC_HLE(lib, sceAvPlayerAddSource);
    DC_HLE(lib, sceAvPlayerAddSourceEx);
    DC_HLE(lib, sceAvPlayerStart);
    DC_HLE(lib, sceAvPlayerStartEx);
    DC_HLE(lib, sceAvPlayerStop);
    DC_HLE(lib, sceAvPlayerPause);
    DC_HLE(lib, sceAvPlayerResume);
    DC_HLE(lib, sceAvPlayerIsActive);
    DC_HLE(lib, sceAvPlayerCurrentTime);
    DC_HLE(lib, sceAvPlayerGetAudioData);
    DC_HLE(lib, sceAvPlayerGetVideoData);
    DC_HLE(lib, sceAvPlayerGetVideoDataEx);
    DC_HLE(lib, sceAvPlayerGetStreamInfo);
    DC_HLE(lib, sceAvPlayerGetStreamInfoEx);
    DC_HLE(lib, sceAvPlayerEnableStream);
    DC_HLE(lib, sceAvPlayerDisableStream);
    DC_HLE(lib, sceAvPlayerChangeStream);
    DC_HLE(lib, sceAvPlayerSetLooping);
    DC_HLE(lib, sceAvPlayerSetAvSyncMode);
    DC_HLE(lib, sceAvPlayerPostInit);
    DC_HLE(lib, sceAvPlayerSetLogCallback);
    DC_HLE(lib, sceAvPlayerSetTrickSpeed);
    DC_HLE(lib, sceAvPlayerStreamCount);
    DC_HLE(lib, sceAvPlayerJumpToTime);
}

} // namespace Hle::AvPlayer
