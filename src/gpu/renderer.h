#pragma once

// Backend-agnostic boundary between the guest GPU frontend (PM4 command
// processor for the PS4 "Liverpool" GPU) and a host graphics API.
//
// The frontend decodes command buffers the game submits through
// sceGnmSubmitCommandBuffers / sceGnmDingDong, tracks register state, and
// calls into this interface only for work that touches the host GPU.
// Backends: Metal (macOS). A Vulkan backend for Windows/Linux can sit behind
// the same interface later.

#include <cstdint>

namespace Gpu {

namespace Liverpool {
class RegFile;
}

using GuestAddr = std::uint64_t;

// Per-draw state that comes from draw packets rather than registers.
struct DrawCall {
    bool indexed = false;
    std::uint32_t count = 0;         // indices or vertices
    std::uint32_t instances = 1;
    GuestAddr index_base = 0;        // INDEX_BASE packet
    std::uint32_t index_offset = 0;  // in indices (DRAW_INDEX_OFFSET_2)
    std::uint32_t index_size = 2;    // bytes per index: 2 or 4
};

// What sceVideoOutSubmitFlip asks to show.
struct FlipInfo {
    GuestAddr address = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0; // pixels
    std::uint32_t pixel_format = 0;
    std::uint32_t tiling_mode = 0;
};

class Renderer {
public:
    virtual ~Renderer() = default;

    // Draws using the current guest register state.
    virtual void Draw(const Liverpool::RegFile& regs, const DrawCall& draw) = 0;
    virtual void Dispatch(const Liverpool::RegFile& regs, std::uint32_t x, std::uint32_t y,
                          std::uint32_t z) = 0;

    // End of one guest submission: hand recorded work to the host GPU.
    virtual void OnSubmit() = 0;

    // Blocks until all recorded work has executed. Called before the guest
    // can observe GPU progress (fences, labels), since it may then reuse the
    // memory that work reads.
    virtual void WaitIdle() = 0;

    // Called on the GPU thread once the frame's command buffers are done.
    virtual void Flip(const FlipInfo& flip) = 0;
};

} // namespace Gpu
