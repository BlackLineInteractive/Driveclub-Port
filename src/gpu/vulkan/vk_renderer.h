#pragma once

// Vulkan backend: shadPS4's renderer (third_party/shadps4) behind our
// Gpu::Renderer interface. Our command processor stays the front end.
// Targets Windows/Linux; on macOS it runs through MoltenVK (DC_RENDERER=vulkan).

#include <memory>
#include <string>

#include "gpu/renderer.h"

struct SDL_Window;

namespace DcVulkan {
class Backend;
}

namespace Gpu::Vulkan {

class VulkanRenderer : public Gpu::Renderer {
public:
    // `metal_layer`: the window's CAMetalLayer on macOS, null elsewhere.
    VulkanRenderer(SDL_Window* window, void* metal_layer, bool vsync);
    ~VulkanRenderer() override;

    void Draw(const Liverpool::RegFile& regs, const DrawCall& draw) override;
    void Dispatch(const Liverpool::RegFile& regs, std::uint32_t x, std::uint32_t y,
                  std::uint32_t z) override;
    void OnSubmit() override;
    void WaitIdle() override;
    void Signal(std::function<void()> done) override;
    void NoteConstDump(std::uint64_t addr, std::uint32_t bytes) override;
    void Flip(const FlipInfo& flip) override;
    std::uint8_t* Gds() override;
    void OnContextRegs(const std::uint32_t* body, std::uint32_t body_dwords) override;
    bool DmaData(std::uint32_t dst_sel, std::uint64_t dst, std::uint32_t src_sel,
                 std::uint64_t src, std::uint32_t bytes) override;

    // Main thread.
    bool Present();
    void Resize(int width_px, int height_px);
    std::string DeviceName() const;

private:
    std::unique_ptr<DcVulkan::Backend> backend;
};

} // namespace Gpu::Vulkan
