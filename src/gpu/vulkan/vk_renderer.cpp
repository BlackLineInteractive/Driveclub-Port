#include "gpu/vulkan/vk_renderer.h"

#include "dc_vulkan.h"
#include "gpu/liverpool/registers.h"

namespace Gpu::Vulkan {

static_assert(Liverpool::RegFile::kSize >= 0xD000, "the backend reads the full register map");

VulkanRenderer::VulkanRenderer(SDL_Window* window, void* metal_layer, bool vsync)
    : backend(DcVulkan::Backend::Create(window, metal_layer, vsync)) {}

VulkanRenderer::~VulkanRenderer() = default;

void VulkanRenderer::Draw(const Liverpool::RegFile& regs, const DrawCall& d) {
    DcVulkan::DrawArgs a;
    a.indexed = d.indexed;
    a.count = d.count;
    a.instances = d.instances;
    a.index_base = d.index_base;
    a.index_offset = d.index_offset;
    a.index_size = d.index_size;
    a.first_vertex = d.first_vertex;
    backend->Draw(regs.Data(), a);
}

void VulkanRenderer::Dispatch(const Liverpool::RegFile& regs, std::uint32_t x, std::uint32_t y,
                              std::uint32_t z) {
    backend->Dispatch(regs.Data(), x, y, z);
}

void VulkanRenderer::OnSubmit() {
    backend->OnSubmit();
}

void VulkanRenderer::WaitIdle() {
    backend->WaitIdle();
}

void VulkanRenderer::Signal(std::function<void()> done) {
    backend->Signal(std::move(done));
}

// The buffer cache tracks CPU writes to GPU memory with page protection, so
// a reused constant-engine dump is picked up like any other write.
void VulkanRenderer::NoteConstDump(std::uint64_t, std::uint32_t) {}

void VulkanRenderer::Flip(const FlipInfo& f) {
    backend->Flip({f.address, f.width, f.height, f.pitch, f.pixel_format, f.tiling_mode});
}

std::uint8_t* VulkanRenderer::Gds() {
    return backend->Gds();
}

void VulkanRenderer::OnContextRegs(const std::uint32_t* body, std::uint32_t body_dwords) {
    backend->OnContextRegs(body, body_dwords);
}

bool VulkanRenderer::DmaData(std::uint32_t dst_sel, std::uint64_t dst, std::uint32_t src_sel,
                             std::uint64_t src, std::uint32_t bytes) {
    return backend->DmaData(dst_sel, dst, src_sel, src, bytes);
}

bool VulkanRenderer::Present() {
    return backend->Present();
}

void VulkanRenderer::Resize(int width_px, int height_px) {
    backend->Resize(width_px, height_px);
}

std::string VulkanRenderer::DeviceName() const {
    return backend->DeviceName();
}

} // namespace Gpu::Vulkan
