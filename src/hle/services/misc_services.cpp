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
alignas(8) static void* s_empty_usb_list[1] = {nullptr};

s32 sceUsbdInit() {
    return 0;
}

void sceUsbdExit() {
}

s64 sceUsbdGetDeviceList(void*** list) {
    if (list) {
        *list = s_empty_usb_list;
    }
    return 0; // 0 devices
}

void sceUsbdFreeDeviceList(void** list, s32 unref_devices) {
    (void)list;
    (void)unref_devices;
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

// libSceAppContent
s32 sceAppContentInitialize(const void* init_param, const void* boot_param) {
    DC_INFO("sceAppContentInitialize()");
    return 0;
}

s32 sceAppContentAppParamGetInt(s32 paramId, s32* out_value) {
    if (out_value) {
        *out_value = 0; // SKU_FLAG_FULL etc.
    }
    return 0;
}

s32 sceAppContentAddcontUnmount() {
    return 0;
}

s32 sceAppContentGetAddcontInfoList(u32 service_label, void* list, u32 list_num, u32* hit_num) {
    if (hit_num) {
        *hit_num = 0;
    }
    return 0;
}

s32 sceAppContentAppParamGetString(s32 paramId, char* buf, size_t buf_size) {
    if (buf && buf_size > 0) {
        buf[0] = '\0';
    }
    return 0;
}

// libSceAudioIn
s32 sceAudioInOpen(s32 user_id, s32 port_type, s32 index, u32 len, u32 freq, u32 param) {
    return 1;
}

s32 sceAudioInClose(s32 handle) {
    return 0;
}

s32 sceAudioInGetSilentState(s32 handle, s32* state) {
    if (state) {
        *state = 0;
    }
    return 0;
}

// libSceAudioOut
s32 sceAudioOutInit() {
    DC_INFO("sceAudioOutInit()");
    return 0;
}

s32 sceAudioOutOpen(s32 user_id, s32 port_type, s32 index, u32 len, u32 freq, u32 param) {
    DC_INFO("sceAudioOutOpen(user_id=%#x, port_type=%d, len=%u, freq=%u) -> handle 1",
            user_id, port_type, len, freq);
    return 1;
}

s32 sceAudioOutClose(s32 handle) {
    return 0;
}

s32 sceAudioOutOutput(s32 handle, const void* ptr) {
    return 0;
}

s32 sceAudioOutOutputs(void* param, u32 num) {
    return 0;
}

s32 sceAudioOutSetVolume(s32 handle, s32 flag, const s32* vol) {
    return 0;
}

s32 sceAudioOutGetPortInfo(s32 handle, void* info) {
    return 0;
}

// libSceAjm
s32 sceAjmInitialize(s64 reserved, u32* p_ctx) {
    if (p_ctx) {
        *p_ctx = 1;
    }
    DC_INFO("sceAjmInitialize() -> ctx 1");
    return 0;
}

s32 sceAjmModuleRegister(u32 ctx, u32 type, s64 reserved) {
    return 0;
}

s32 sceAjmInstanceCreate(u32 ctx, u32 type, u32 flags, u32* out_inst) {
    if (out_inst) {
        *out_inst = 1;
    }
    return 0;
}

s32 sceAjmInstanceDestroy(u32 ctx, u32 inst) {
    return 0;
}

s32 sceAjmMemoryRegister(u32 ctx, void* ptr, size_t num_pages) {
    return 0;
}

s32 sceAjmMemoryUnregister(u32 ctx, void* ptr) {
    return 0;
}

s32 sceAjmBatchJobControlBufferRa() {
    return 0;
}

s32 sceAjmBatchJobRunBufferRa() {
    return 0;
}

s32 sceAjmBatchStartBuffer() {
    return 0;
}

s32 sceAjmBatchWait(u32 context_id, u32 batch_id, s64 timeout, void* result) {
    return 0;
}

// libSceHttp & libSceSsl
s32 sceHttpInit(s32 pool_id, s32 ssl_id, size_t pool_size) {
    return 0;
}

s32 sceHttpCreateEpoll(s32 pool_id, const char* name) {
    return 1;
}

s32 sceHttpCreateTemplate(s32 pool_id, const char* user_agent, s32 version, s32 auto_redirect) {
    return 1;
}

s32 sceHttpGetMemoryPoolStats(s32 pool_id, void* stats) {
    return 0;
}

s32 sceHttpWaitRequest(s32 req_id, void* result) {
    return 0;
}

s32 sceHttpsGetCaList(void* ca_list) {
    return 0;
}

s32 sceHttpSetConnectTimeOut(s32 id, u32 timeout) {
    return 0;
}

s32 sceHttpSetSendTimeOut(s32 id, u32 timeout) {
    return 0;
}

s32 sceHttpSetRecvTimeOut(s32 id, u32 timeout) {
    return 0;
}

s32 sceHttpSetResolveTimeOut(s32 id, u32 timeout) {
    return 0;
}

s32 sceHttpSetNonblock(s32 id, u32 enable) {
    return 0;
}

s32 sceHttpSetCookieEnabled(s32 id, u32 enable) {
    return 0;
}

s32 sceHttpAddRequestHeader(s32 id, const char* name, const char* value, u32 mode) {
    return 0;
}

s32 sceHttpsSetSslCallback(s32 id, void* cb, void* user_data) {
    return 0;
}

s32 sceSslInit(size_t pool_size) {
    return 1;
}

s32 sceSslGetMemoryPoolStats(s32 pool_id, void* stats) {
    return 0;
}

// libSceVoice
s32 sceVoiceInit(void* mem, size_t mem_size, void* params) {
    DC_INFO("sceVoiceInit()");
    return 0;
}

s32 sceVoiceEnd() {
    return 0;
}

s32 sceVoiceCreatePort(u32* port_out, const void* param) {
    if (port_out) {
        *port_out = 1;
    }
    return 0;
}

s32 sceVoiceDeletePort(u32 port) {
    return 0;
}

s32 sceVoiceStart(u32 port) {
    return 0;
}

s32 sceVoiceStop(u32 port) {
    return 0;
}

s32 sceVoiceConnectIPortToOPort(u32 iport, u32 oport) {
    return 0;
}

s32 sceVoiceDisconnectIPortFromOPort(u32 iport, u32 oport) {
    return 0;
}

s32 sceVoiceWriteToIPort(u32 port, const void* data) {
    return 0;
}

s32 sceVoiceReadFromOPort(u32 port, void* data) {
    return 0;
}

s32 sceVoiceGetPortInfo(u32 port, void* info) {
    return 0;
}

// Dialogs
s32 sceErrorDialogUpdateStatus(void* status) {
    return 0;
}

s32 sceMsgDialogUpdateStatus(void* status) {
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
    DC_HLE("libSceUsbd", sceUsbdExit);
    DC_HLE("libSceUsbd", sceUsbdGetDeviceList);
    DC_HLE("libSceUsbd", sceUsbdFreeDeviceList);
    DC_HLE("libSceUsbd", sceUsbdHandleEventsTimeout);

    DC_HLE("libSceAppContent", sceAppContentInitialize);
    DC_HLE("libSceAppContent", sceAppContentAppParamGetInt);
    DC_HLE("libSceAppContent", sceAppContentAddcontUnmount);
    DC_HLE("libSceAppContent", sceAppContentGetAddcontInfoList);
    DC_HLE("libSceAppContent", sceAppContentAppParamGetString);

    DC_HLE("libSceAudioIn", sceAudioInOpen);
    DC_HLE("libSceAudioIn", sceAudioInClose);
    DC_HLE("libSceAudioIn", sceAudioInGetSilentState);

    DC_HLE("libSceAudioOut", sceAudioOutInit);
    DC_HLE("libSceAudioOut", sceAudioOutOpen);
    DC_HLE("libSceAudioOut", sceAudioOutClose);
    DC_HLE("libSceAudioOut", sceAudioOutOutput);
    DC_HLE("libSceAudioOut", sceAudioOutOutputs);
    DC_HLE("libSceAudioOut", sceAudioOutSetVolume);
    DC_HLE("libSceAudioOut", sceAudioOutGetPortInfo);

    DC_HLE("libSceAjm", sceAjmInitialize);
    DC_HLE("libSceAjm", sceAjmModuleRegister);
    DC_HLE("libSceAjm", sceAjmInstanceCreate);
    DC_HLE("libSceAjm", sceAjmInstanceDestroy);
    DC_HLE("libSceAjm", sceAjmMemoryRegister);
    DC_HLE("libSceAjm", sceAjmMemoryUnregister);
    DC_HLE("libSceAjm", sceAjmBatchJobControlBufferRa);
    DC_HLE("libSceAjm", sceAjmBatchJobRunBufferRa);
    DC_HLE("libSceAjm", sceAjmBatchStartBuffer);
    DC_HLE("libSceAjm", sceAjmBatchWait);

    DC_HLE("libSceVoice", sceVoiceInit);
    DC_HLE("libSceVoice", sceVoiceEnd);
    DC_HLE("libSceVoice", sceVoiceCreatePort);
    DC_HLE("libSceVoice", sceVoiceDeletePort);
    DC_HLE("libSceVoice", sceVoiceStart);
    DC_HLE("libSceVoice", sceVoiceStop);
    DC_HLE("libSceVoice", sceVoiceConnectIPortToOPort);
    DC_HLE("libSceVoice", sceVoiceDisconnectIPortFromOPort);
    DC_HLE("libSceVoice", sceVoiceWriteToIPort);
    DC_HLE("libSceVoice", sceVoiceReadFromOPort);
    DC_HLE("libSceVoice", sceVoiceGetPortInfo);

    DC_HLE("libSceHttp", sceHttpInit);
    DC_HLE("libSceHttp", sceHttpCreateEpoll);
    DC_HLE("libSceHttp", sceHttpCreateTemplate);
    DC_HLE("libSceHttp", sceHttpGetMemoryPoolStats);
    DC_HLE("libSceHttp", sceHttpWaitRequest);
    DC_HLE("libSceHttp", sceHttpsGetCaList);
    DC_HLE("libSceHttp", sceHttpSetConnectTimeOut);
    DC_HLE("libSceHttp", sceHttpSetSendTimeOut);
    DC_HLE("libSceHttp", sceHttpSetRecvTimeOut);
    DC_HLE("libSceHttp", sceHttpSetResolveTimeOut);
    DC_HLE("libSceHttp", sceHttpSetNonblock);
    DC_HLE("libSceHttp", sceHttpSetCookieEnabled);
    DC_HLE("libSceHttp", sceHttpAddRequestHeader);
    DC_HLE("libSceHttp", sceHttpsSetSslCallback);
    DC_HLE("libSceSsl", sceHttpsSetSslCallback);

    DC_HLE("libSceSsl", sceSslInit);
    DC_HLE("libSceSsl", sceSslGetMemoryPoolStats);

    DC_HLE("libSceErrorDialog", sceErrorDialogUpdateStatus);
    DC_HLE("libSceMsgDialog", sceMsgDialogUpdateStatus);
}

} // namespace Hle::MiscServices
