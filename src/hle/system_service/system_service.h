#pragma once

#include "common/types.h"

namespace Hle::SystemService {

struct OrbisSystemServiceStatus {
    s32 event_num;
    bool is_system_ui_overlaid;
    bool is_in_background_execution;
    bool is_cpu_mode7_cpu_normal;
    bool is_game_live_streaming_on_air;
    bool is_out_of_vr_play_area;
    u8 reserved[26];
};

struct OrbisSystemServiceDisplaySafeAreaInfo {
    float ratio;
    u8 reserved[128];
};

struct OrbisSystemServiceEvent {
    u32 event_type;
    u8 data[8192];
};

void RegisterSystemService();

} // namespace Hle::SystemService
