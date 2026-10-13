#pragma once

// The shadPS4 Vulkan renderer as seen from Driveclub Native.
//
// Our PM4 command processor (src/gpu/liverpool) stays the GPU front end; this
// interface hands it the shadPS4 rasterizer, texture/buffer caches, shader
// recompiler and a swapchain presenter. Plain C++ only: see dc_host.h for why.
// Implemented in src/gpu/vulkan/backend/dc_vulkan.cpp.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct SDL_Window;

namespace DcVulkan {

struct DrawArgs {
    bool indexed = false;
    std::uint32_t count = 0;
    std::uint32_t instances = 1;
    std::uint64_t index_base = 0;
    std::uint32_t index_offset = 0;
    std::uint32_t index_size = 2;
    std::uint32_t first_vertex = 0;
};

struct FlipArgs {
    std::uint64_t address = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0; // pixels
    std::uint32_t pixel_format = 0;
    std::uint32_t tiling_mode = 0;
};

class Backend {
public:
    // `metal_layer` is the window's CAMetalLayer on macOS, null elsewhere.
    static std::unique_ptr<Backend> Create(SDL_Window* window, void* metal_layer, bool vsync);
    virtual ~Backend() = default;

    virtual std::string DeviceName() const = 0;

    // ---- GPU thread (the command processor) ----
    // `regs` is the full hardware register file (dword index = register offset).
    virtual void Draw(const std::uint32_t* regs, const DrawArgs& args) = 0;
    virtual void Dispatch(const std::uint32_t* regs, std::uint32_t x, std::uint32_t y,
                          std::uint32_t z) = 0;
    // A SET_CONTEXT_REG packet body; the Gnm driver hides surface size hints
    // in the NOP packet that follows CB/DB setup.
    virtual void OnContextRegs(const std::uint32_t* body, std::uint32_t body_dwords) = 0;
    // CP DMA. sel: 0 memory, 1 GDS, 2 immediate data (src only). Returns
    // false if the CP should do it itself.
    virtual bool DmaData(std::uint32_t dst_sel, std::uint64_t dst, std::uint32_t src_sel,
                         std::uint64_t src, std::uint32_t bytes) = 0;
    virtual void OnSubmit() = 0;
    virtual void WaitIdle() = 0;
    virtual void Signal(std::function<void()> done) = 0;
    virtual void Flip(const FlipArgs& flip) = 0;
    virtual std::uint8_t* Gds() = 0;

    // ---- main thread ----
    // Presents the latest flipped frame, if there is a new one. Returns false
    // when nothing was presented.
    virtual bool Present() = 0;
    virtual void Resize(int width_px, int height_px) = 0;
};

} // namespace DcVulkan
