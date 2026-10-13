#pragma once

#include <ctime>

#include "common/types.h"

namespace Hle::Rtc {

struct OrbisRtcTick {
    u64 tick;
};

struct OrbisRtcDateTime {
    u16 year;
    u16 month;
    u16 day;
    u16 hour;
    u16 minute;
    u16 second;
    u32 microsecond;
};

void RegisterRtc();

} // namespace Hle::Rtc
