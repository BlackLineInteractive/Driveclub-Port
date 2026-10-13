// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "gpu/renderer.h"

namespace Gpu::Metal {

class Device;

class MetalRenderer : public Gpu::Renderer {
public:
    explicit MetalRenderer(Device& device);
    ~MetalRenderer() override;

    void Draw(const Liverpool::Regs& regs, bool indexed) override;
    void DrawIndirect(const Liverpool::Regs& regs, bool indexed, GuestAddr args,
                      std::uint32_t draw_count, GuestAddr count_addr) override;

    void Dispatch(const Liverpool::Regs& regs, std::uint32_t x, std::uint32_t y,
                  std::uint32_t z) override;
    void DispatchIndirect(const Liverpool::Regs& regs, GuestAddr args) override;

    void CopyBuffer(GuestAddr dst, GuestAddr src, std::uint64_t size) override;
    void FillBuffer(GuestAddr dst, std::uint32_t value, std::uint64_t size) override;

    void InvalidateMemory(GuestAddr addr, std::uint64_t size) override;
    void FlushMemory(GuestAddr addr, std::uint64_t size) override;

    void OnSubmit() override;
    void SignalFence(GuestAddr addr, std::uint64_t value, bool interrupt) override;

    void Flip(GuestAddr framebuffer, std::uint32_t width, std::uint32_t height,
              std::uint32_t pitch, std::uint32_t format) override;

    void PushMarker(const char* name) override;
    void PopMarker() override;

    // Returns the latest rendered framebuffer texture as an id<MTLTexture> cast to void*.
    void* GetLatestTexture() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gpu::Metal
