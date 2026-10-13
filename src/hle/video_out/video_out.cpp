#include "hle/video_out/video_out.h"

#include <cstring>
#include <mutex>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::VideoOut {

namespace {

constexpr s32 kErrorInvalidHandle = -2134704127;  // 0x80C90001
constexpr s32 kErrorInvalidAddress = -2134704126; // 0x80C90002
constexpr s32 kErrorInvalidEvent = -2134704124;   // 0x80C90004

// Internal event id stored in kevent.ident
constexpr u64 kInternalEventFlip = 0x6;
constexpr u64 kInternalEventVblank = 0x7;

// Public event id returned by sceVideoOutGetEventId
constexpr s32 kPublicEventFlip = 0;
constexpr s32 kPublicEventVblank = 1;

} // namespace

Driver::Driver() {
    vblank_thread = std::jthread([this](std::stop_token stop) {
        pthread_setname_np("VideoOut Vblank");
        constexpr auto kInterval = std::chrono::microseconds(16667); // ~60 Hz
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            next += kInterval;
            std::this_thread::sleep_until(next);

            struct TriggerItem {
                Kernel::EventQueue* eq;
                s64 data;
            };
            std::vector<TriggerItem> triggers;
            {
                std::scoped_lock lk(lock);
                for (size_t i = 1; i < ports.size(); ++i) {
                    Port& p = ports[i];
                    if (!p.open) continue;
                    ++p.vblank_count;
                    p.vblank_status.count = p.vblank_count;
                    p.vblank_status.process_time =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                    const s64 data = (p.vblank_count << 16) | kInternalEventVblank;
                    for (auto* eq : p.vblank_queues) {
                        triggers.push_back({eq, data});
                    }
                }
            }
            for (const auto& item : triggers) {
                item.eq->Trigger(kInternalEventVblank, Kernel::kEvfiltVideoOut, item.data);
            }
        }
    });
}

s32 Driver::Open(s32 bus_type, s32 index) {
    std::scoped_lock lk(lock);
    for (size_t i = 1; i < ports.size(); ++i) {
        if (!ports[i].open) {
            ports[i].open = true;
            ports[i].labels.fill(0);
            DC_INFO("video_out: opened port %zu (bus %d, index %d)", i, bus_type, index);
            return static_cast<s32>(i);
        }
    }
    return kErrorInvalidHandle;
}

s32 Driver::RegisterBuffers(s32 handle, s32 start_index, void* const* addresses, s32 num,
                            const BufferAttribute* attr) {
    std::scoped_lock lk(lock);
    if (handle <= 0 || static_cast<size_t>(handle) >= ports.size() || !ports[handle].open) {
        return kErrorInvalidHandle;
    }
    if (!addresses || !attr || start_index < 0 || start_index + num > static_cast<s32>(kMaxBuffers)) {
        return kErrorInvalidAddress;
    }

    Port& p = ports[handle];
    p.attr = *attr;
    for (s32 i = 0; i < num; ++i) {
        p.buffers[start_index + i] = addresses[i];
        p.labels[start_index + i] = 0;
    }
    DC_INFO("video_out: registered %d buffers at %dx%d on port %d", num, attr->width, attr->height,
            handle);
    return 0;
}

s32 Driver::SetFlipRate(s32 handle, s32 rate) {
    std::scoped_lock lk(lock);
    if (handle <= 0 || static_cast<size_t>(handle) >= ports.size() || !ports[handle].open) {
        return kErrorInvalidHandle;
    }
    ports[handle].flip_rate = rate;
    return 0;
}

s32 Driver::AddFlipEvent(Kernel::EventQueue* eq, s32 handle, void* udata) {
    std::scoped_lock lk(lock);
    if (handle <= 0 || static_cast<size_t>(handle) >= ports.size() || !ports[handle].open) {
        return kErrorInvalidHandle;
    }
    if (!eq) {
        return kErrorInvalidAddress;
    }
    eq->Add(kInternalEventFlip, Kernel::kEvfiltVideoOut, udata);
    ports[handle].flip_queues.push_back(eq);
    return 0;
}

s32 Driver::AddVblankEvent(Kernel::EventQueue* eq, s32 handle, void* udata) {
    std::scoped_lock lk(lock);
    if (handle <= 0 || static_cast<size_t>(handle) >= ports.size() || !ports[handle].open) {
        return kErrorInvalidHandle;
    }
    if (!eq) {
        return kErrorInvalidAddress;
    }
    eq->Add(kInternalEventVblank, Kernel::kEvfiltVideoOut, udata);
    ports[handle].vblank_queues.push_back(eq);
    return 0;
}

uintptr_t Driver::GetBufferLabelAddress(s32 handle) {
    std::scoped_lock lk(lock);
    if (handle <= 0 || static_cast<size_t>(handle) >= ports.size() || !ports[handle].open) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(ports[handle].labels.data());
}

void Driver::MarkFlipPending(s32 handle, u32 buf_idx) {
    // Note: Do not mark label = 1 here. The command buffer itself may check
    // WaitFlipDone(buf_idx) at start of frame; marking it pending before execution
    // causes a deadlock. The displayed buffer is tracked in OnFlipDone.
    (void)handle;
    (void)buf_idx;
}

void Driver::OnFlipDone(s32 handle, u32 buf_idx, s64 flip_arg) {
    std::vector<Kernel::EventQueue*> queues;
    s64 data = 0;
    {
        std::scoped_lock lk(lock);
        if (handle <= 0 || static_cast<size_t>(handle) >= ports.size()) {
            return;
        }
        Port& p = ports[handle];
        if (buf_idx < kMaxBuffers) {
            // Free the previous buffer that was being scanned out
            if (p.prev_buffer >= 0 && static_cast<size_t>(p.prev_buffer) < kMaxBuffers) {
                p.labels[p.prev_buffer] = 0;
            }
            // Mark the currently displayed buffer as busy
            p.labels[buf_idx] = 1;
            p.prev_buffer = static_cast<s32>(buf_idx);

            if (p.buffers[buf_idx]) {
                latest_framebuffer.ptr = p.buffers[buf_idx];
                latest_framebuffer.width = p.attr.width;
                latest_framebuffer.height = p.attr.height;
                latest_framebuffer.pitch = p.attr.pitch_in_pixel;
                latest_framebuffer.pixel_format = p.attr.pixel_format;
                latest_framebuffer.flip_count = p.flip_count + 1;
            }
        }
        ++p.flip_count;
        queues = p.flip_queues;
        data = (flip_arg << 16) | (p.flip_count & 0xFFFF);
    }
    for (auto* eq : queues) {
        eq->Trigger(kInternalEventFlip, Kernel::kEvfiltVideoOut, data);
    }
}

FramebufferInfo Driver::GetLatestFramebuffer() {
    std::scoped_lock lk(lock);
    return latest_framebuffer;
}

s32 Driver::GetVblankStatus(s32 handle, SceVideoOutVblankStatus* status) {
    if (!status) {
        return kErrorInvalidAddress;
    }
    std::scoped_lock lk(lock);
    if (handle <= 0 || static_cast<size_t>(handle) >= ports.size() || !ports[handle].open) {
        return kErrorInvalidHandle;
    }
    *status = ports[handle].vblank_status;
    return 0;
}

Driver& Driver::Instance() {
    static Driver instance;
    return instance;
}

namespace {

s32 sceVideoOutOpen(u32 /*userId*/, s32 bus_type, s32 index, const void* /*param*/) {
    return Driver::Instance().Open(bus_type, index);
}

void sceVideoOutSetBufferAttribute(BufferAttribute* attr, u32 pixel_format, s32 tiling_mode,
                                    s32 aspect_ratio, u32 width, u32 height, u32 pitch) {
    if (!attr) {
        return;
    }
    attr->pixel_format = pixel_format;
    attr->tiling_mode = tiling_mode;
    attr->aspect_ratio = aspect_ratio;
    attr->width = width;
    attr->height = height;
    attr->pitch_in_pixel = pitch;
    attr->option = 0;
    attr->reserved0 = 0;
    attr->reserved1 = 0;
}

s32 sceVideoOutRegisterBuffers(s32 handle, s32 start_index, void* const* addresses, s32 num,
                                const BufferAttribute* attr) {
    return Driver::Instance().RegisterBuffers(handle, start_index, addresses, num, attr);
}

s32 sceVideoOutSetFlipRate(s32 handle, s32 rate) {
    return Driver::Instance().SetFlipRate(handle, rate);
}

s32 sceVideoOutSetWindowModeMargins(s32 /*handle*/, s32 /*top*/, s32 /*bottom*/, s32 /*left*/,
                                     s32 /*right*/) {
    return 0;
}

s32 sceVideoOutGetResolutionStatus(s32 handle, SceVideoOutResolutionStatus* status) {
    if (!status) {
        return kErrorInvalidAddress;
    }
    *status = SceVideoOutResolutionStatus{};
    return 0;
}

s32 sceVideoOutGetVblankStatus(s32 handle, SceVideoOutVblankStatus* status) {
    return Driver::Instance().GetVblankStatus(handle, status);
}

s32 sceVideoOutAddFlipEvent(Kernel::EventQueue* eq, s32 handle, void* udata) {
    return Driver::Instance().AddFlipEvent(eq, handle, udata);
}

s32 sceVideoOutAddVblankEvent(Kernel::EventQueue* eq, s32 handle, void* udata) {
    return Driver::Instance().AddVblankEvent(eq, handle, udata);
}

s32 sceVideoOutGetEventId(const Kernel::GuestKevent* ev) {
    if (!ev) {
        return kErrorInvalidAddress;
    }
    if (ev->filter != Kernel::kEvfiltVideoOut) {
        return kErrorInvalidEvent;
    }
    if (ev->ident == kInternalEventFlip) {
        return kPublicEventFlip;
    }
    if (ev->ident == kInternalEventVblank) {
        return kPublicEventVblank;
    }
    return -1;
}

s32 sceVideoOutGetEventData(const Kernel::GuestKevent* ev, s64* data) {
    if (!ev || !data) {
        return kErrorInvalidAddress;
    }
    if (ev->filter != Kernel::kEvfiltVideoOut) {
        return kErrorInvalidEvent;
    }
    const s64 ev_data = ev->data >> 16;
    if (ev->ident != kInternalEventFlip || ev->data >= 0) {
        *data = ev_data;
    } else {
        *data = ev_data | static_cast<s64>(0xFFFF000000000000ULL);
    }
    return 0;
}

} // namespace

void RegisterVideoOut() {
    Hle::Register("libSceVideoOut", "Up36PTk687E", reinterpret_cast<void*>(sceVideoOutOpen));
    Hle::Register("libSceVideoOut", "i6-sR91Wt-4", reinterpret_cast<void*>(sceVideoOutSetBufferAttribute));
    Hle::Register("libSceVideoOut", "w3BY+tAEiQY", reinterpret_cast<void*>(sceVideoOutRegisterBuffers));
    Hle::Register("libSceVideoOut", "CBiu4mCE1DA", reinterpret_cast<void*>(sceVideoOutSetFlipRate));
    Hle::Register("libSceVideoOut", "MTxxrOCeSig", reinterpret_cast<void*>(sceVideoOutSetWindowModeMargins));
    Hle::Register("libSceVideoOut", "6kPnj51T62Y", reinterpret_cast<void*>(sceVideoOutGetResolutionStatus));
    Hle::Register("libSceVideoOut", "1FZBKy8HeNU", reinterpret_cast<void*>(sceVideoOutGetVblankStatus));
    Hle::Register("libSceVideoOut", "HXzjK9yI30k", reinterpret_cast<void*>(sceVideoOutAddFlipEvent));
    Hle::Register("libSceVideoOut", "Xru92wHJRmg", reinterpret_cast<void*>(sceVideoOutAddVblankEvent));
    Hle::Register("libSceVideoOut", "U2JJtSqNKZI", reinterpret_cast<void*>(sceVideoOutGetEventId));
    Hle::Register("libSceVideoOut", "rWUTcKdkUzQ", reinterpret_cast<void*>(sceVideoOutGetEventData));
}

} // namespace Hle::VideoOut
