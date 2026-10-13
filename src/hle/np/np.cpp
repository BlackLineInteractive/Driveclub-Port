// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/np/np.h"

#include <cstring>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Np {

namespace {

// Error codes
constexpr s32 kErrorNpSignedOut = static_cast<s32>(0x80550006);

// libSceNpManager
s32 sceNpGetNpId(s32 user_id, void* np_id) {
    if (np_id) {
        std::memset(np_id, 0, 32);
        std::strncpy(reinterpret_cast<char*>(np_id), "Player", 16);
    }
    DC_INFO("sceNpGetNpId(user_id=%#x) -> Player", user_id);
    return 0;
}

s32 sceNpGetOnlineId(s32 user_id, void* online_id) {
    if (online_id) {
        std::memset(online_id, 0, 20);
        std::strncpy(reinterpret_cast<char*>(online_id), "Player", 16);
    }
    DC_INFO("sceNpGetOnlineId(user_id=%#x) -> Player", user_id);
    return 0;
}

s32 sceNpCheckCallback() {
    return 0;
}

s32 sceNpCreateRequest() {
    return 1;
}

s32 sceNpCreateAsyncRequest(const void* param) {
    (void)param;
    static s32 next_req = 1;
    return next_req++;
}

s32 sceNpDeleteRequest(s32 req_id) {
    return 0;
}

s32 sceNpPollAsync(s32 req_id, s32* result) {
    (void)req_id;
    if (result) {
        *result = 0;
    }
    return 0;
}

s32 sceNpWaitAsync(s32 req_id, s32* result) {
    (void)req_id;
    if (result) {
        *result = 0;
    }
    return 0;
}

s32 sceNpAbortRequest(s32 req_id) {
    return 0;
}

s32 sceNpCheckPlus(s32 req_id, const void* param, void* result) {
    (void)req_id;
    (void)param;
    if (result) {
        *reinterpret_cast<u8*>(result) = 1; // authorized
    }
    return 0;
}

s32 sceNpSetContentRestriction(const void* param) {
    return 0;
}

s32 sceNpSetNpTitleId(const void* title_id, const void* secret) {
    return 0;
}

s32 sceNpRegisterStateCallback(void* cb, void* arg) {
    return 0;
}

s32 sceNpCheckNpAvailability(s32 user_id) {
    (void)user_id;
    return 0;
}

s32 sceNpNotifyPlusFeature(s32 user_id, s32 feature, s64 reserved) {
    return 0;
}

s32 sceNpGetAccountLanguage(s32 user_id, char* lang) {
    if (lang) {
        std::strncpy(lang, "en", 3);
    }
    return 0;
}

s32 sceNpGetAccountCountry(s32 user_id, char* country) {
    if (country) {
        std::strncpy(country, "us", 3);
    }
    return 0;
}

s32 sceNpGetAccountDateOfBirth(s32 user_id, void* dob) {
    return 0;
}

// libSceNpUtility
s32 sceNpLookupCreateTitleCtx(const void* self_np_id) {
    DC_INFO("sceNpLookupCreateTitleCtx()");
    return 1;
}

s32 sceNpLookupCreateTitleCtxA(s32 user_id) {
    DC_INFO("sceNpLookupCreateTitleCtxA(user_id=%#x)", user_id);
    return 1;
}

s32 sceNpLookupCreateAsyncRequest(s32 title_ctx_id) {
    return 1;
}

s32 sceNpLookupNpId(s32 req_id, const void* online_id, void* np_id) {
    return 0;
}

// libSceNpWebApi
s32 sceNpWebApiInitialize(s32 user_id, void* param) {
    DC_INFO("sceNpWebApiInitialize(user_id=%#x)", user_id);
    return 0;
}

s32 sceNpWebApiCreateHandle() {
    return 1;
}

s32 sceNpWebApiDeleteHandle(s32 handle) {
    return 0;
}

s32 sceNpWebApiAbortRequest(s32 req_id) {
    return 0;
}

s32 sceNpWebApiRegisterPushEventCallback() {
    return 0;
}

s32 sceNpWebApiDeleteServicePushEventFilter() {
    return 0;
}

s32 sceNpWebApiReadData() {
    return 0;
}

// libSceNpTrophy
s32 sceNpTrophyCreateContext(void* ctx, s32 user_id, const void* title_id, s64 reserved) {
    return 0;
}

s32 sceNpTrophyDestroyContext(s32 ctx) {
    return 0;
}

s32 sceNpTrophyCreateHandle(void* handle) {
    return 0;
}

s32 sceNpTrophyDestroyHandle(s32 handle) {
    return 0;
}

s32 sceNpTrophyRegisterContext(s32 ctx, s32 handle, s64 reserved) {
    return 0;
}

s32 sceNpTrophyGetTrophyUnlockState(s32 ctx, s32 handle, void* flags, void* count) {
    return 0;
}

s32 sceNpTrophyUnlockTrophy(s32 ctx, s32 handle, s32 trophy_id, void* platinum) {
    return 0;
}

// libSceNpSignaling
s32 sceNpSignalingInitialize() {
    return 0;
}

s32 sceNpSignalingTerminate() {
    return 0;
}

s32 sceNpSignalingCreateContext() {
    return 1;
}

s32 sceNpSignalingActivateConnection() {
    return 0;
}

s32 sceNpSignalingGetPeerNetInfoResult() {
    return 0;
}

s32 sceNpSignalingGetConnectionInfo() {
    return 0;
}

s32 sceNpSignalingGetLocalNetInfo() {
    return 0;
}

// libSceNpTus
s32 sceNpTusCreateRequest() {
    return 1;
}

s32 sceNpTusDeleteNpTitleCtx() {
    return 0;
}

s32 sceNpTssCreateNpTitleCtx(u32 service_label, void* np_id) {
    (void)service_label;
    (void)np_id;
    return 1;
}

s32 sceNpTssCreateNpTitleCtxA(u32 service_label, s32 user_id) {
    (void)service_label;
    (void)user_id;
    return 1;
}

s32 sceNpTssGetSmallStorage(s32 req_id, void* data, u64 max_size, u64* content_len, void* option) {
    (void)req_id;
    (void)data;
    (void)max_size;
    (void)option;
    if (content_len) {
        *content_len = 0;
    }
    return 0;
}

// libSceNpAuth
s32 sceNpAuthCreateRequest() {
    return 1;
}

s32 sceNpAuthCreateAsyncRequest() {
    return 1;
}

s32 sceNpAuthDeleteRequest() {
    return 0;
}

s32 sceNpAuthGetAuthorizationCode() {
    return kErrorNpSignedOut;
}

s32 sceNpAuthWaitAsync() {
    return 0;
}

// Dialogs
s32 sceNpProfileDialogInitialize() {
    return 0;
}

s32 sceNpProfileDialogTerminate() {
    return 0;
}

s32 sceNpProfileDialogGetResult() {
    return 0;
}

s32 sceNpCommerceDialogInitialize() {
    return 0;
}

s32 sceNpCommerceDialogOpen() {
    return 0;
}

s32 sceNpCommerceDialogUpdateStatus() {
    return 0;
}

s32 sceNpCommerceDialogGetStatus() {
    return 0;
}

s32 sceNpSnsFacebookDialogOpen() {
    return 0;
}

s32 sceNpSnsFacebookDialogUpdateStatus() {
    return 0;
}

s32 sceNpSnsFacebookDialogGetResult() {
    return 0;
}

s32 sceNpPartyInitialize() {
    return 0;
}

s32 sceNpPartyRegisterHandler() {
    return 0;
}

s32 sceNpPartyGetState(void* state) {
    return 0;
}

s32 sceNpPartyCheckCallback() {
    return 0;
}

} // namespace

void RegisterNp() {
    constexpr const char* kNpManager = "libSceNpManager";
    DC_HLE(kNpManager, sceNpGetNpId);
    DC_HLE(kNpManager, sceNpGetOnlineId);
    DC_HLE(kNpManager, sceNpCheckCallback);
    DC_HLE(kNpManager, sceNpCreateRequest);
    DC_HLE(kNpManager, sceNpCreateAsyncRequest);
    DC_HLE(kNpManager, sceNpPollAsync);
    DC_HLE(kNpManager, sceNpWaitAsync);
    DC_HLE(kNpManager, sceNpDeleteRequest);
    DC_HLE(kNpManager, sceNpAbortRequest);
    DC_HLE(kNpManager, sceNpSetContentRestriction);
    DC_HLE(kNpManager, sceNpSetNpTitleId);
    DC_HLE(kNpManager, sceNpRegisterStateCallback);
    DC_HLE(kNpManager, sceNpCheckNpAvailability);
    DC_HLE(kNpManager, sceNpCheckPlus);
    DC_HLE(kNpManager, sceNpNotifyPlusFeature);
    DC_HLE(kNpManager, sceNpGetAccountLanguage);
    DC_HLE(kNpManager, sceNpGetAccountCountry);
    DC_HLE(kNpManager, sceNpGetAccountDateOfBirth);

    constexpr const char* kNpUtility = "libSceNpUtility";
    DC_HLE(kNpUtility, sceNpLookupCreateTitleCtx);
    DC_HLE(kNpUtility, sceNpLookupCreateTitleCtxA);
    DC_HLE(kNpUtility, sceNpLookupCreateAsyncRequest);
    DC_HLE(kNpUtility, sceNpLookupNpId);

    constexpr const char* kNpWebApi = "libSceNpWebApi";
    DC_HLE(kNpWebApi, sceNpWebApiInitialize);
    DC_HLE(kNpWebApi, sceNpWebApiCreateHandle);
    DC_HLE(kNpWebApi, sceNpWebApiDeleteHandle);
    DC_HLE(kNpWebApi, sceNpWebApiAbortRequest);
    DC_HLE(kNpWebApi, sceNpWebApiRegisterPushEventCallback);
    DC_HLE(kNpWebApi, sceNpWebApiDeleteServicePushEventFilter);
    DC_HLE(kNpWebApi, sceNpWebApiReadData);

    constexpr const char* kNpTrophy = "libSceNpTrophy";
    DC_HLE(kNpTrophy, sceNpTrophyCreateContext);
    DC_HLE(kNpTrophy, sceNpTrophyDestroyContext);
    DC_HLE(kNpTrophy, sceNpTrophyCreateHandle);
    DC_HLE(kNpTrophy, sceNpTrophyDestroyHandle);
    DC_HLE(kNpTrophy, sceNpTrophyRegisterContext);
    DC_HLE(kNpTrophy, sceNpTrophyGetTrophyUnlockState);
    DC_HLE(kNpTrophy, sceNpTrophyUnlockTrophy);

    constexpr const char* kNpSignaling = "libSceNpSignaling";
    DC_HLE(kNpSignaling, sceNpSignalingInitialize);
    DC_HLE(kNpSignaling, sceNpSignalingTerminate);
    DC_HLE(kNpSignaling, sceNpSignalingCreateContext);
    DC_HLE(kNpSignaling, sceNpSignalingActivateConnection);
    DC_HLE(kNpSignaling, sceNpSignalingGetPeerNetInfoResult);
    DC_HLE(kNpSignaling, sceNpSignalingGetConnectionInfo);
    DC_HLE(kNpSignaling, sceNpSignalingGetLocalNetInfo);

    constexpr const char* kNpTus = "libSceNpTus";
    DC_HLE(kNpTus, sceNpTusCreateRequest);
    DC_HLE(kNpTus, sceNpTusDeleteNpTitleCtx);
    DC_HLE(kNpTus, sceNpTssCreateNpTitleCtx);
    DC_HLE(kNpTus, sceNpTssCreateNpTitleCtxA);
    DC_HLE(kNpTus, sceNpTssGetSmallStorage);

    constexpr const char* kNpAuth = "libSceNpAuth";
    DC_HLE(kNpAuth, sceNpAuthCreateRequest);
    DC_HLE(kNpAuth, sceNpAuthCreateAsyncRequest);
    DC_HLE(kNpAuth, sceNpAuthDeleteRequest);
    DC_HLE(kNpAuth, sceNpAuthGetAuthorizationCode);
    DC_HLE(kNpAuth, sceNpAuthWaitAsync);

    constexpr const char* kNpProfileDialog = "libSceNpProfileDialog";
    DC_HLE(kNpProfileDialog, sceNpProfileDialogInitialize);
    DC_HLE(kNpProfileDialog, sceNpProfileDialogTerminate);
    DC_HLE(kNpProfileDialog, sceNpProfileDialogGetResult);

    constexpr const char* kNpCommerceDialog = "libSceNpCommerceDialog";
    DC_HLE(kNpCommerceDialog, sceNpCommerceDialogInitialize);
    DC_HLE(kNpCommerceDialog, sceNpCommerceDialogOpen);
    DC_HLE(kNpCommerceDialog, sceNpCommerceDialogUpdateStatus);
    DC_HLE(kNpCommerceDialog, sceNpCommerceDialogGetStatus);

    constexpr const char* kNpSnsFacebookDialog = "libSceNpSnsFacebookDialog";
    DC_HLE(kNpSnsFacebookDialog, sceNpSnsFacebookDialogOpen);
    DC_HLE(kNpSnsFacebookDialog, sceNpSnsFacebookDialogUpdateStatus);
    DC_HLE(kNpSnsFacebookDialog, sceNpSnsFacebookDialogGetResult);

    constexpr const char* kNpParty = "libSceNpParty";
    DC_HLE(kNpParty, sceNpPartyInitialize);
    DC_HLE(kNpParty, sceNpPartyRegisterHandler);
    DC_HLE(kNpParty, sceNpPartyGetState);
    DC_HLE(kNpParty, sceNpPartyCheckCallback);
}

} // namespace Hle::Np
