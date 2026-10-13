// DcVulkan::Backend (bridge/dc_vulkan.h): the shadPS4 Vulkan renderer behind
// our command processor.
//
// Built with the shadPS4 include root. shadPS4's own PM4 front end and
// ImGui-based presenter are not used: draws and dispatches arrive here with
// our register file, which is mirrored into AmdGpu::Liverpool::regs (the
// layouts are both the hardware register map), and flipped frames are
// post-processed with shadPS4's pass and blitted to the swapchain. The
// presenter follows shadPS4's Presenter::PrepareFrame/Present.

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/videoout/buffer.h"
#include "core/memory.h"
#include "core/signals.h"
#include "dc_host.h"
#include "dc_vulkan.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace DcVulkan {

namespace {

using namespace Vulkan;
using AmdGpu::Regs;

constexpr u32 kNumFrames = 3;

// Register ranges the rasterizer reads (copied per draw; the rest of the
// 0xD000-dword file is either unused or set from the draw packet below).
struct RegRange {
    u32 begin, end;
};
constexpr RegRange kDrawRegs[] = {
    {0x2C00, 0x2D80}, // SH: PS, VS, GS, ES, HS, LS programs
    {0xA000, 0xA400}, // context
    {0xC240, 0xC260}, // uconfig: VGT primitive type, ring sizes, tess factor base
};
constexpr u32 kComputeRegBase = 0x2E00; // COMPUTE_DISPATCH_INITIATOR

vk::Format FrameViewFormat(Libraries::VideoOut::PixelFormat format) {
    using Libraries::VideoOut::PixelFormat;
    switch (format) {
    case PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    case PixelFormat::A8R8G8B8Srgb:
    default:
        return vk::Format::eB8G8R8A8Srgb;
    }
}

// Runs completion callbacks once the GPU passes their tick, in order.
class Waiter {
public:
    explicit Waiter(Semaphore& sema) : sema{sema}, thread{[this](std::stop_token st) { Run(st); }} {}

    ~Waiter() {
        thread.request_stop();
        cv.notify_all();
    }

    void Push(u64 tick, std::function<void()> fn) {
        {
            std::scoped_lock lk{lock};
            queue.emplace_back(tick, std::move(fn));
        }
        cv.notify_one();
    }

private:
    void Run(std::stop_token st) {
        Common::SetCurrentThreadName("GPU fences");
        while (true) {
            std::pair<u64, std::function<void()>> job;
            {
                std::unique_lock lk{lock};
                cv.wait(lk, [&] { return !queue.empty() || st.stop_requested(); });
                if (st.stop_requested()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }
            sema.Wait(job.first);
            job.second();
        }
    }

    Semaphore& sema;
    std::mutex lock;
    std::condition_variable cv;
    std::deque<std::pair<u64, std::function<void()>>> queue;
    std::jthread thread;
};

class BackendImpl final : public Backend {
public:
    BackendImpl(SDL_Window* sdl_window, void* metal_layer, bool vsync)
        : window{(EmulatorSettings.SetPresentMode(vsync ? "Fifo" : "Mailbox"), sdl_window),
                 metal_layer},
          instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
                   EmulatorSettings.IsVkCrashDiagnosticEnabled()},
          draw_scheduler{instance}, present_scheduler{instance}, swapchain{instance, window},
          runtime{instance, draw_scheduler},
          rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, &liverpool)},
          texture_cache{rasterizer->GetTextureCache()},
          waiter{std::make_unique<Waiter>(*draw_scheduler.GetWorkSemaphore())} {
        // Installs the write-watch fault handler before any page is watched.
        Core::Signals::Instance();
        liverpool.regs.SetDefaults();

        const vk::Device device = instance.GetDevice();
        frames.resize(kNumFrames);
        for (u32 i = 0; i < kNumFrames; ++i) {
            Frame& frame = frames[i];
            frame.id = static_cast<u8>(i);
            frame.present_done = Check<"create present done fence">(
                device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
            free_frames.push_back(&frame);
        }
        pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
        LOG_INFO(Render_Vulkan, "shadPS4 Vulkan renderer on {}", instance.GetModelName());
    }

    ~BackendImpl() override {
        std::scoped_lock lk{liverpool.dc_gpu_lock};
        waiter.reset();
        draw_scheduler.Finish();
        present_scheduler.Finish();
        const vk::Device device = instance.GetDevice();
        for (auto& frame : frames) {
            if (frame.image_view) {
                device.destroyImageView(frame.image_view);
            }
            if (frame.image) {
                vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
            }
            device.destroyFence(frame.present_done);
        }
    }

    std::string DeviceName() const override {
        return std::string(instance.GetModelName());
    }

    // ================= GPU thread =================

    void Draw(const u32* regs, const DrawArgs& a) override {
        std::scoped_lock lk{liverpool.dc_gpu_lock};
        EnterGpuThread();
        auto& r = liverpool.regs;
        for (const auto& range : kDrawRegs) {
            std::memcpy(&r.reg_array[range.begin], &regs[range.begin],
                        (range.end - range.begin) * sizeof(u32));
        }
        // State that comes from draw packets rather than registers.
        r.num_indices = a.count;
        r.num_instances.num_instances = a.instances;
        if (a.indexed) {
            r.index_base_address.base_addr_lo = static_cast<u32>(a.index_base);
            r.index_base_address.base_addr_hi = static_cast<u32>(a.index_base >> 32);
            r.index_buffer_type.raw = a.index_size == 4 ? 1u : 0u;
            r.max_index_size = a.index_offset + a.count;
        }
        rasterizer->Draw(a.indexed, a.index_offset);
    }

    void Dispatch(const u32* regs, u32 x, u32 y, u32 z) override {
        std::scoped_lock lk{liverpool.dc_gpu_lock};
        EnterGpuThread();
        auto& cs = liverpool.DcComputeState();
        std::memcpy(&cs, &regs[kComputeRegBase], sizeof(cs));
        cs.dim_x = x;
        cs.dim_y = y;
        cs.dim_z = z;
        rasterizer->DispatchDirect();
    }

    // Mirrors shadPS4's SET_CONTEXT_REG handling of the Gnm size hints.
    void OnContextRegs(const u32* body, u32 n) override {
        if (n < 2) {
            return;
        }
        using L = AmdGpu::Liverpool;
        const u32 reg = Regs::ContextRegWordOffset + body[0];
        const u32* payload = body + 1;
        const u32 count = n - 1; // register values (shadPS4's type3.count + 1 - 1)
        const u32 nop_offset = n - 1;
        std::scoped_lock lk{liverpool.dc_gpu_lock};
        if (reg >= L::CbColor0Base && reg <= L::CbColor7Base &&
            (reg - L::CbColor0Base) % (L::CbColor1Base - L::CbColor0Base) == 0) {
            const u32 cb = (reg - L::CbColor0Base) / (L::CbColor1Base - L::CbColor0Base);
            if ((count == 0x0e || count == 0x0d || count == 0x0b) &&
                body[nop_offset + 1] == 0xc0001000) {
                liverpool.last_cb_extent[cb].raw = body[nop_offset + 2];
            } else {
                liverpool.last_cb_extent[cb].raw = 0;
            }
        } else if (reg >= L::CbColor0Cmask && reg <= L::CbColor7Cmask &&
                   (reg - L::CbColor0Cmask) % (L::CbColor1Cmask - L::CbColor0Cmask) == 0) {
            const u32 cb = (reg - L::CbColor0Cmask) / (L::CbColor1Cmask - L::CbColor0Cmask);
            if (count == 0x04 && body[nop_offset + 1] == 0xc0001000) {
                liverpool.last_cb_extent[cb].raw = body[nop_offset + 2];
            }
        } else if (reg == L::DbZInfo) {
            if (count == 8 && payload[20] == 0xc0001000) {
                liverpool.last_db_extent.raw = payload[21];
            } else {
                liverpool.last_db_extent.raw = 0;
            }
        }
    }

    bool DmaData(u32 dst_sel, u64 dst, u32 src_sel, u64 src, u32 bytes) override {
        std::scoped_lock lk{liverpool.dc_gpu_lock};
        EnterGpuThread();
        const bool dst_gds = dst_sel == 1;
        if (src_sel == 2) {
            rasterizer->FillBuffer(dst, bytes, static_cast<u32>(src), dst_gds);
        } else {
            rasterizer->CopyBuffer(dst, src, bytes, dst_gds, src_sel == 1);
        }
        return true;
    }

    void OnSubmit() override {
        std::scoped_lock lk{liverpool.dc_gpu_lock};
        EnterGpuThread();
        rasterizer->OnSubmit();
        rasterizer->Flush();
    }

    void WaitIdle() override {
        std::scoped_lock lk{liverpool.dc_gpu_lock};
        EnterGpuThread();
        rasterizer->Finish();
    }

    void Signal(std::function<void()> done) override {
        u64 tick;
        {
            std::scoped_lock lk{liverpool.dc_gpu_lock};
            EnterGpuThread();
            rasterizer->OnFence();
            tick = rasterizer->Flush();
        }
        waiter->Push(tick, std::move(done));
    }

    void Flip(const FlipArgs& f) override {
        std::scoped_lock lk{liverpool.dc_gpu_lock};
        EnterGpuThread();
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.is_occupied = true;
        group.attrib.pixel_format = static_cast<Libraries::VideoOut::PixelFormat>(f.pixel_format);
        group.attrib.tiling_mode = static_cast<Libraries::VideoOut::TilingMode>(f.tiling_mode);
        group.attrib.width = f.width;
        group.attrib.height = f.height;
        group.attrib.pitch_in_pixel = f.pitch;
        Frame* frame = PrepareFrame(group, f.address);
        {
            std::scoped_lock fl{frame_lock};
            if (ready_frame) { // never shown: superseded
                free_frames.push_back(ready_frame);
            }
            ready_frame = frame;
        }
        frame_cv.notify_all();
        if (++flips <= 3 || flips % 300 == 0) {
            LOG_INFO(Render_Vulkan, "flip #{} {:#x} {}x{}", flips, f.address, f.width, f.height);
        }
    }

    u8* Gds() override {
        return gds.data(); // GDS lives on the GPU; DmaData handles CP access
    }

    // ================= main thread =================

    bool Present() override {
        Frame* frame;
        {
            std::scoped_lock fl{frame_lock};
            if (ready_frame) {
                if (shown_frame) {
                    free_frames.push_back(shown_frame);
                }
                shown_frame = ready_frame;
                ready_frame = nullptr;
            }
            frame = shown_frame;
        }
        frame_cv.notify_all();
        if (!frame) {
            return false;
        }

        const u32 win_w = static_cast<u32>(window.GetWidth());
        const u32 win_h = static_cast<u32>(window.GetHeight());
        if (win_w == 0 || win_h == 0) {
            return false;
        }
        if (win_w != swapchain.GetWidth() || win_h != swapchain.GetHeight()) {
            swapchain.Recreate(win_w, win_h);
        }
        if (!swapchain.AcquireNextImage()) {
            swapchain.Recreate(win_w, win_h);
            if (!swapchain.AcquireNextImage()) {
                return false;
            }
        }
        // The previous present of this frame has to be done before it is reused.
        WaitFence(frame->present_done);
        Check(instance.GetDevice().resetFences(frame->present_done));

        const vk::Image target = swapchain.Image();
        const vk::Extent2D extent = swapchain.GetExtent();
        const auto cmdbuf = present_scheduler.CommandBuffer();
        const vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const std::array pre{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = target,
                .subresourceRange = color,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .image = frame->image,
                .subresourceRange = color,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(pre.size()),
            .pImageMemoryBarriers = pre.data(),
        });
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(target, vk::ImageLayout::eTransferDstOptimal, black, color);

        // Letterbox: keep the game's aspect ratio.
        const float scale = std::min(float(extent.width) / float(frame->width),
                                     float(extent.height) / float(frame->height));
        const s32 w = std::max<s32>(1, s32(float(frame->width) * scale));
        const s32 h = std::max<s32>(1, s32(float(frame->height) * scale));
        const s32 x = (s32(extent.width) - w) / 2, y = (s32(extent.height) - h) / 2;
        const vk::ImageBlit blit{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                     vk::Offset3D{s32(frame->width), s32(frame->height), 1}},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstOffsets = std::array{vk::Offset3D{x, y, 0}, vk::Offset3D{x + w, y + h, 1}},
        };
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, target,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

        const std::array post{
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .image = target,
                .subresourceRange = color,
            },
            vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eNone,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = frame->image,
                .subresourceRange = color,
            },
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(post.size()),
            .pImageMemoryBarriers = post.data(),
        });

        SubmitInfo info{};
        info.AddWait(swapchain.GetImageAcquiredSemaphore());
        info.AddWait(frame->ready_semaphore, frame->ready_tick);
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        info.AddSignal(frame->present_done);
        present_scheduler.Flush(info);
        {
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            if (!swapchain.Present()) {
                swapchain.Recreate(win_w, win_h);
            }
        }
        return true;
    }

    void Resize(int width_px, int height_px) override {
        window.SetSize(width_px, height_px);
    }

private:
    void EnterGpuThread() {
        if (!gpu_thread_set) {
            liverpool.DcSetGpuThread();
            gpu_thread_set = true;
        }
        SyncMappings();
    }

    // Tells the caches which guest ranges are GPU memory. Our memory manager
    // has no rasterizer callbacks, so diff its direct mappings instead.
    void SyncMappings() {
        const u64 gen = DcHost::DirectGeneration();
        if (gen == mappings_gen) {
            return;
        }
        mappings_gen = gen;
        std::vector<DcHost::DirectRange> now = DcHost::DirectRanges();
        const auto key = [](const DcHost::DirectRange& r) { return std::pair{r.va, r.size}; };
        std::ranges::sort(now, {}, key);
        for (const auto& r : mappings) {
            if (!std::ranges::binary_search(now, key(r), {}, key)) {
                rasterizer->UnmapMemory(r.va, r.size);
            }
        }
        for (const auto& r : now) {
            if (!std::ranges::binary_search(mappings, key(r), {}, key) &&
                r.va + r.size < 0x10000000000ull) {
                rasterizer->RegisterMemory(r.va, r.size);
                rasterizer->MapMemory(r.va, r.size);
            }
        }
        mappings = std::move(now);
    }

    void WaitFence(vk::Fence fence) {
        while (true) {
            const vk::Result result =
                instance.GetDevice().waitForFences(fence, false, std::numeric_limits<u64>::max());
            if (result == vk::Result::eSuccess) {
                return;
            }
            ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost waiting for a frame");
        }
    }

    Frame* GetRenderFrame(u32 width, u32 height) {
        Frame* frame;
        {
            std::unique_lock fl{frame_lock};
            frame_cv.wait(fl, [&] { return !free_frames.empty(); });
            frame = free_frames.front();
            free_frames.pop_front();
        }
        WaitFence(frame->present_done);
        if (frame->width != width || frame->height != height) {
            RecreateFrame(frame, width, height);
        }
        return frame;
    }

    void RecreateFrame(Frame* frame, u32 width, u32 height) {
        const vk::Device device = instance.GetDevice();
        if (frame->image_view) {
            device.destroyImageView(frame->image_view);
        }
        if (frame->image) {
            vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
        }
        const vk::Format format = swapchain.GetSurfaceFormat().format;
        const vk::ImageCreateInfo image_info = {
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eSampled,
        };
        const VmaAllocationCreateInfo alloc_info = {
            .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VkImage image{};
        const VkImageCreateInfo raw_info = static_cast<VkImageCreateInfo>(image_info);
        const VkResult result = vmaCreateImage(instance.GetAllocator(), &raw_info, &alloc_info,
                                               &image, &frame->allocation, nullptr);
        ASSERT_MSG(result == VK_SUCCESS, "Failed allocating a frame image: {}",
                   vk::to_string(vk::Result{result}));
        frame->image = vk::Image{image};
        frame->image_view = Check<"create frame image view">(device.createImageView({
            .image = frame->image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        frame->width = width;
        frame->height = height;
    }

    // shadPS4 Presenter::PrepareFrame without FSR, screenshots and ImGui.
    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr address) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, address};
        const auto image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);

        Frame* frame = GetRenderFrame(attribute.attrib.width, attribute.attrib.height);

        const auto pre_barrier = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = frame->image,
            .subresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        draw_scheduler.EndRendering();
        const auto cmdbuf = draw_scheduler.CommandBuffer();
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        });

        VideoCore::ImageViewInfo view_info{};
        view_info.format = FrameViewFormat(attribute.attrib.pixel_format);
        view_info.mapping.a = AmdGpu::CompSwizzle::One; // scan-out ignores alpha
        auto& image = texture_cache.GetImage(image_id);
        const vk::ImageView image_view = *image.FindView(view_info).image_view;
        const vk::Extent2D image_size = {image.info.size.width, image.info.size.height};

        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();

        HostPasses::PostProcessingPass::Settings pp{};
        // Vulkan has no sRGB 10-bit format: the pass decodes those itself.
        pp.srgb_input =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
        pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp);

        frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
        frame->ready_tick = draw_scheduler.CurrentTick();
        SubmitInfo info{};
        draw_scheduler.Flush(info);
        return frame;
    }

    Frontend::WindowSDL window;
    Instance instance;
    Scheduler draw_scheduler;
    Scheduler present_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    AmdGpu::Liverpool liverpool;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    std::unique_ptr<Waiter> waiter;
    HostPasses::PostProcessingPass pp_pass;

    bool gpu_thread_set = false;
    u64 mappings_gen = ~0ull;
    std::vector<DcHost::DirectRange> mappings;
    std::array<u8, 64_KB> gds{};
    u64 flips = 0;

    // Frames: free -> (GPU thread) ready -> (main thread) shown -> free.
    std::vector<Frame> frames;
    std::mutex frame_lock;
    std::condition_variable frame_cv;
    std::deque<Frame*> free_frames;
    Frame* ready_frame = nullptr;
    Frame* shown_frame = nullptr;
};

} // namespace

std::unique_ptr<Backend> Backend::Create(SDL_Window* window, void* metal_layer, bool vsync) {
    return std::make_unique<BackendImpl>(window, metal_layer, vsync);
}

} // namespace DcVulkan
