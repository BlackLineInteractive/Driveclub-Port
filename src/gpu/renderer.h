#pragma once

// Backend-agnostic boundary between the guest GPU frontend (PM4 command
// processor for the PS4 "Liverpool" GPU) and a host graphics API.
//
// The frontend decodes command buffers the game submits through
// sceGnmSubmitCommandBuffers / sceGnmDingDong, tracks register state, and
// calls into this interface only for work that touches the host GPU.
// Backends: Metal (macOS, ours). A Vulkan backend for Windows/Linux can be
// built on shadPS4's renderer behind the same interface.

#include <cstdint>

namespace Gpu {

using GuestAddr = std::uint64_t;

// Snapshot of the guest register file the backend needs to build pipelines
// and bind resources. Filled by the frontend; layout is frontend-owned.
struct GuestRegs;

class Renderer {
public:
    virtual ~Renderer() = default;

    // Draws using the current guest register state.
    virtual void Draw(const GuestRegs& regs, bool indexed) = 0;
    virtual void DrawIndirect(const GuestRegs& regs, bool indexed, GuestAddr args,
                              std::uint32_t draw_count, GuestAddr count_addr) = 0;

    virtual void Dispatch(const GuestRegs& regs, std::uint32_t x, std::uint32_t y,
                          std::uint32_t z) = 0;
    virtual void DispatchIndirect(const GuestRegs& regs, GuestAddr args) = 0;

    // DMA-style operations issued by PM4 DMA_DATA / WRITE_DATA packets.
    virtual void CopyBuffer(GuestAddr dst, GuestAddr src, std::uint64_t size) = 0;
    virtual void FillBuffer(GuestAddr dst, std::uint32_t value, std::uint64_t size) = 0;

    // The guest CPU wrote to [addr, addr+size); drop or re-upload GPU copies.
    virtual void InvalidateMemory(GuestAddr addr, std::uint64_t size) = 0;
    // The guest CPU is about to read [addr, addr+size); download GPU results.
    virtual void FlushMemory(GuestAddr addr, std::uint64_t size) = 0;

    // Submission boundaries and EOP/EOS fences (sceGnmSubmitDone, EVENT_WRITE_EOP).
    virtual void OnSubmit() = 0;
    virtual void SignalFence(GuestAddr addr, std::uint64_t value, bool interrupt) = 0;

    // sceVideoOutSubmitFlip: show the guest framebuffer at `addr`.
    virtual void Flip(GuestAddr framebuffer, std::uint32_t width, std::uint32_t height,
                      std::uint32_t pitch, std::uint32_t format) = 0;

    // Debug markers (sceGnmInsertPushMarker etc.); no-ops in release builds.
    virtual void PushMarker(const char* name) = 0;
    virtual void PopMarker() = 0;
};

} // namespace Gpu
