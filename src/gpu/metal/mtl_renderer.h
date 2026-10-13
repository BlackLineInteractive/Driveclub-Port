#pragma once

// Metal backend for the guest GPU: translates guest shaders to MSL, keeps
// render targets and textures as Metal textures, and records draws.

#include <memory>

#include "gpu/renderer.h"

namespace Gpu::Metal {

class Device;

class MetalRenderer : public Gpu::Renderer {
public:
    explicit MetalRenderer(Device& device);
    ~MetalRenderer() override;

    void Draw(const Liverpool::RegFile& regs, const DrawCall& draw) override;
    void Dispatch(const Liverpool::RegFile& regs, std::uint32_t x, std::uint32_t y,
                  std::uint32_t z) override;
    void OnSubmit() override;
    void WaitIdle() override;
    void Signal(std::function<void()> done) override;
    void NoteConstDump(std::uint64_t addr, std::uint32_t bytes) override;
    void Flip(const FlipInfo& flip) override;
    std::uint8_t* Gds() override;

    // Main thread: the last flipped frame as id<MTLTexture> (void*), or null.
    // It stays valid until two more flips have happened.
    void* LatestFrame() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Gpu::Metal
