// Driveclub Native: replaces shadPS4's vk_presenter.h, whose presenter draws
// the game through Dear ImGui. Our presenter lives in
// src/gpu/vulkan/backend/dc_vulkan.cpp; the post-processing pass only needs
// the Frame it renders into.

#pragma once

#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

struct Frame {
    u32 width{};
    u32 height{};
    VmaAllocation allocation{};
    vk::Image image{};
    vk::ImageView image_view{};
    vk::Fence present_done{};
    vk::Semaphore ready_semaphore{};
    u64 ready_tick{};
    bool is_hdr{false};
    u8 id{};
};

} // namespace Vulkan
