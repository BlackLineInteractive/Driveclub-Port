// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/rtc/rtc.h"

#include <chrono>
#include <cstring>
#include <ctime>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Rtc {

namespace {

constexpr u64 kUnixEpochTicks = 0x00dcddb30f2f8000ULL;

s32 sceRtcGetCurrentNetworkTick(OrbisRtcTick* pTick) {
    if (!pTick) {
        return -1;
    }
    const auto now = std::chrono::system_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    pTick->tick = static_cast<u64>(us) + kUnixEpochTicks;
    return 0;
}

s32 sceRtcGetTime_t(const OrbisRtcTick* pTick, time_t* outTime) {
    if (!pTick || !outTime) {
        return -1;
    }
    if (pTick->tick >= kUnixEpochTicks) {
        *outTime = static_cast<time_t>((pTick->tick - kUnixEpochTicks) / 1000000ULL);
    } else {
        *outTime = 0;
    }
    return 0;
}

s32 sceRtcSetTick(OrbisRtcDateTime* pTime, const OrbisRtcTick* pTick) {
    if (!pTime || !pTick) {
        return -1;
    }
    time_t sec = 0;
    if (pTick->tick >= kUnixEpochTicks) {
        sec = static_cast<time_t>((pTick->tick - kUnixEpochTicks) / 1000000ULL);
    }
    std::tm tm{};
    gmtime_r(&sec, &tm);
    pTime->year = static_cast<u16>(tm.tm_year + 1900);
    pTime->month = static_cast<u16>(tm.tm_mon + 1);
    pTime->day = static_cast<u16>(tm.tm_mday);
    pTime->hour = static_cast<u16>(tm.tm_hour);
    pTime->minute = static_cast<u16>(tm.tm_min);
    pTime->second = static_cast<u16>(tm.tm_sec);
    pTime->microsecond = static_cast<u32>(pTick->tick % 1000000ULL);
    return 0;
}

} // namespace

void RegisterRtc() {
    constexpr const char* lib = "libSceRtc";
    DC_HLE(lib, sceRtcGetCurrentNetworkTick);
    DC_HLE(lib, sceRtcGetTime_t);
    DC_HLE(lib, sceRtcSetTick);
}

} // namespace Hle::Rtc
