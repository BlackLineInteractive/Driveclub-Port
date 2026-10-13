// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/user_service/user_service.h"

#include <cstdio>
#include <cstring>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::UserService {

namespace {

s32 sceUserServiceInitialize(const void* params) {
    DC_INFO("sceUserServiceInitialize()");
    return 0;
}

s32 sceUserServiceTerminate() {
    DC_INFO("sceUserServiceTerminate()");
    return 0;
}

s32 sceUserServiceGetInitialUser(s32* outUserId) {
    if (outUserId) {
        *outUserId = kInitialUserId;
    }
    DC_INFO("sceUserServiceGetInitialUser() -> %#x", kInitialUserId);
    return 0;
}

s32 sceUserServiceGetLoginUserIdList(s32* outUserIdList) {
    if (outUserIdList) {
        outUserIdList[0] = kInitialUserId;
        outUserIdList[1] = -1;
        outUserIdList[2] = -1;
        outUserIdList[3] = -1;
    }
    DC_INFO("sceUserServiceGetLoginUserIdList() -> [%#x, -1, -1, -1]", kInitialUserId);
    return 0;
}

s32 sceUserServiceGetUserName(s32 userId, char* outUserName, size_t size) {
    if (outUserName && size > 0) {
        std::snprintf(outUserName, size, "Player");
    }
    DC_INFO("sceUserServiceGetUserName(userId=%#x) -> Player", userId);
    return 0;
}

} // namespace

void RegisterUserService() {
    constexpr const char* lib = "libSceUserService";
    DC_HLE(lib, sceUserServiceInitialize);
    DC_HLE(lib, sceUserServiceTerminate);
    DC_HLE(lib, sceUserServiceGetInitialUser);
    DC_HLE(lib, sceUserServiceGetLoginUserIdList);
    DC_HLE(lib, sceUserServiceGetUserName);
}

} // namespace Hle::UserService
