#pragma once

// Backend-agnostic boundary between the guest GPU frontend (PM4 command
// processor for the PS4 "Liverpool" GPU) and a host graphics API.
//
// The frontend decodes command buffers the game submits through
// sceGnmSubmitCommandBuffers / sceGnmDingDong, tracks register state, and
// calls into this interface only for work that touches the host GPU.
// Backends: Metal (macOS, src/gpu/metal) and Vulkan (Windows/Linux, also
// usable on macOS through MoltenVK; src/gpu/vulkan wraps shadPS4's renderer).

#include <cstdint>
#include <functional>

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
    std::uint32_t first_vertex = 0;  // non-indexed draws (indirect)
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

    // Blocks until all recorded work has executed (the CP then touches
    // memory that work uses).
    virtual void WaitIdle() = 0;

    // Runs `done` on another thread once all work recorded so far has
    // executed; calls complete in order. Fences, labels and their interrupts
    // go through this, so the guest sees GPU progress without the CP waiting.
    virtual void Signal(std::function<void()> done) = 0;

    // The constant engine wrote `bytes` of resource table at guest `addr`.
    // The guest reuses that memory quickly, so the renderer snapshots dumps
    // that draws point at instead of the CP waiting for the GPU.
    virtual void NoteConstDump(std::uint64_t addr, std::uint32_t bytes) = 0;

    // Called on the GPU thread once the frame's command buffers are done.
    virtual void Flip(const FlipInfo& flip) = 0;

    // A SET_CONTEXT_REG packet body (`body[0]` = register offset), read in
    // place in the command buffer: the Gnm driver puts render target size
    // hints in the NOP packet right after it.
    virtual void OnContextRegs(const std::uint32_t* body, std::uint32_t body_dwords) {}

    // CP DMA (DMA_DATA). sel: 0 memory, 1 GDS, 2 immediate (`src` is the
    // value; source only). Return true if the backend did the copy on the
    // GPU; false lets the CP do it on the CPU.
    virtual bool DmaData(std::uint32_t dst_sel, std::uint64_t dst, std::uint32_t src_sel,
                         std::uint64_t src, std::uint32_t bytes) {
        return false;
    }

    // The 64 KB global data share, shared by shaders and CP DMA (call WaitIdle
    // before reading what shaders wrote).
    virtual std::uint8_t* Gds() = 0;
};

} // namespace Gpu
