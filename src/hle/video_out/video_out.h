#pragma once

#include <array>
#include <vector>

#include "common/types.h"
#include "hle/kernel/equeue.h"

namespace Hle::VideoOut {

struct BufferAttribute {
    u32 pixel_format;
    s32 tiling_mode;
    s32 aspect_ratio;
    u32 width;
    u32 height;
    u32 pitch_in_pixel;
    u32 option;
    u32 reserved0;
    u64 reserved1;
};

struct SceVideoOutResolutionStatus {
    s32 full_width = 1920;
    s32 full_height = 1080;
    s32 pane_width = 1920;
    s32 pane_height = 1080;
    u64 refresh_rate = 3; // 59.94 Hz
    float screen_size_in_inch = 50.0f;
    u16 flags = 0;
    u16 reserved0 = 0;
    u32 reserved1[3] = {0};
};

struct SceVideoOutVblankStatus {
    u64 count = 0;
    u64 process_time = 0;
    u64 tsc = 0;
    u64 reserved[1] = {0};
    u8 flags = 0;
    u8 pad1[7] = {};
};

class Driver {
public:
    static constexpr size_t kMaxBuffers = 16;

    Driver();

    s32 Open(s32 bus_type, s32 index);
    s32 RegisterBuffers(s32 handle, s32 start_index, void* const* addresses, s32 num,
                        const BufferAttribute* attr);
    s32 SetFlipRate(s32 handle, s32 rate);
    s32 AddFlipEvent(Kernel::EventQueue* eq, s32 handle, void* udata);
    s32 AddVblankEvent(Kernel::EventQueue* eq, s32 handle, void* udata);

    uintptr_t GetBufferLabelAddress(s32 handle);
    void MarkFlipPending(s32 handle, u32 buf_idx);
    void OnFlipDone(s32 handle, u32 buf_idx, s64 flip_arg);

    static Driver& Instance();

private:
    struct Port {
        bool open = false;
        s32 flip_rate = 0;
        BufferAttribute attr{};
        std::array<void*, kMaxBuffers> buffers{};
        // Labels for sceGnmInsertWaitFlipDone: 0 = buffer ready/idle, 1 = pending flip
        alignas(64) std::array<u64, kMaxBuffers> labels{};
        u64 flip_count = 0;
        std::vector<Kernel::EventQueue*> flip_queues;
        std::vector<Kernel::EventQueue*> vblank_queues;
    };

    std::mutex lock;
    std::array<Port, 4> ports;
};

void RegisterVideoOut();

} // namespace Hle::VideoOut
