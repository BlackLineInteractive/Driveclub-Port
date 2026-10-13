// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/sysmodule/sysmodule.h"

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Sysmodule {

namespace {

s32 sceSysmoduleLoadModule(u32 id) {
    DC_INFO("sceSysmoduleLoadModule(%#x)", id);
    return 0;
}

s32 sceSysmoduleUnloadModule(u32 id) {
    DC_INFO("sceSysmoduleUnloadModule(%#x)", id);
    return 0;
}

s32 sceSysmoduleIsLoaded(u32 id) {
    return 1;
}

} // namespace

void RegisterSysmodule() {
    constexpr const char* lib = "libSceSysmodule";
    DC_HLE(lib, sceSysmoduleLoadModule);
    DC_HLE(lib, sceSysmoduleUnloadModule);
    DC_HLE(lib, sceSysmoduleIsLoaded);
}

} // namespace Hle::Sysmodule
