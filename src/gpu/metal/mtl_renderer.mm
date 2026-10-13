// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu/metal/mtl_renderer.h"

#import <Metal/Metal.h>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/liverpool/regs.h"
#include "gpu/metal/mtl_device.h"
#include "gpu/metal/mtl_shader_embedded.h"

namespace Gpu::Metal {

struct MetalRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;

    id<MTLCommandBuffer> current_cmd_buf = nil;
    id<MTLRenderCommandEncoder> current_encoder = nil;
    id<MTLTexture> current_render_target = nil;
    GuestAddr current_target_addr = 0;

    std::unordered_map<GuestAddr, id<MTLTexture>> target_textures;
    id<MTLTexture> last_drawn_target = nil;
    mutable std::mutex latest_tex_mutex;
    id<MTLTexture> latest_texture = nil;

    id<MTLRenderPipelineState> pipeline_ui_panel = nil;
    id<MTLRenderPipelineState> pipeline_ui_text = nil;
    id<MTLBuffer> quad_index_buffer = nil;
    id<MTLBuffer> dynamic_vbuf = nil;
    id<MTLTexture> default_white_texture = nil;
    id<MTLSamplerState> default_sampler = nil;
    std::unordered_map<GuestAddr, id<MTLTexture>> guest_textures;

    void InitPipelines() {
        std::vector<uint16_t> indices;
        indices.reserve(16384 * 6);
        for (uint32_t q = 0; q < 16384; ++q) {
            uint16_t b = static_cast<uint16_t>(q * 4);
            indices.push_back(b + 0);
            indices.push_back(b + 1);
            indices.push_back(b + 2);
            indices.push_back(b + 0);
            indices.push_back(b + 2);
            indices.push_back(b + 3);
        }
        quad_index_buffer = [device newBufferWithBytes:indices.data()
                                                length:indices.size() * sizeof(uint16_t)
                                               options:MTLResourceStorageModeShared];
        dynamic_vbuf = [device newBufferWithLength:1024 * 1024 options:MTLResourceStorageModeShared];

        MTLTextureDescriptor* wDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:2
                                        height:2
                                     mipmapped:NO];
        wDesc.usage = MTLTextureUsageShaderRead;
        default_white_texture = [device newTextureWithDescriptor:wDesc];
        uint32_t white_pixels[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
        [default_white_texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2)
                                 mipmapLevel:0
                                   withBytes:white_pixels
                                 bytesPerRow:8];

        MTLSamplerDescriptor* sDesc = [MTLSamplerDescriptor new];
        sDesc.minFilter = MTLSamplerMinMagFilterLinear;
        sDesc.magFilter = MTLSamplerMinMagFilterLinear;
        sDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        default_sampler = [device newSamplerStateWithDescriptor:sDesc];

        NSError* err = nil;
        NSString* vs_src = [NSString stringWithUTF8String:EmbeddedShaders::kShaderVsUI];
        id<MTLLibrary> vs_lib = [device newLibraryWithSource:vs_src options:nil error:&err];
        if (err || !vs_lib) {
            DC_ERROR("metal_renderer: failed to compile vs_ui: %s",
                     [[err localizedDescription] UTF8String]);
        }

        NSString* ps_panel_src = [NSString stringWithUTF8String:EmbeddedShaders::kShaderPsUIPanel];
        id<MTLLibrary> ps_panel_lib = [device newLibraryWithSource:ps_panel_src options:nil error:&err];
        if (err || !ps_panel_lib) {
            DC_ERROR("metal_renderer: failed to compile ps_ui_panel: %s",
                     [[err localizedDescription] UTF8String]);
        }

        NSString* ps_text_src = [NSString stringWithUTF8String:EmbeddedShaders::kShaderPsUIText];
        id<MTLLibrary> ps_text_lib = [device newLibraryWithSource:ps_text_src options:nil error:&err];
        if (err || !ps_text_lib) {
            DC_ERROR("metal_renderer: failed to compile ps_ui_text: %s",
                     [[err localizedDescription] UTF8String]);
        }

        if (vs_lib && ps_panel_lib) {
            id<MTLFunction> vs_fn = [vs_lib newFunctionWithName:@"main0"];
            id<MTLFunction> ps_panel_fn = [ps_panel_lib newFunctionWithName:@"main0"];

            MTLRenderPipelineDescriptor* pDesc = [MTLRenderPipelineDescriptor new];
            pDesc.vertexFunction = vs_fn;
            pDesc.fragmentFunction = ps_panel_fn;
            pDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            pDesc.colorAttachments[0].blendingEnabled = YES;
            pDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

            pipeline_ui_panel = [device newRenderPipelineStateWithDescriptor:pDesc error:&err];
            if (err || !pipeline_ui_panel) {
                DC_ERROR("metal_renderer: failed to create pipeline_ui_panel: %s",
                         [[err localizedDescription] UTF8String]);
            } else {
                DC_INFO("metal_renderer: pipeline_ui_panel initialized successfully");
            }
        }

        if (vs_lib && ps_text_lib) {
            id<MTLFunction> vs_fn = [vs_lib newFunctionWithName:@"main0"];
            id<MTLFunction> ps_text_fn = [ps_text_lib newFunctionWithName:@"main0"];

            MTLRenderPipelineDescriptor* pDesc = [MTLRenderPipelineDescriptor new];
            pDesc.vertexFunction = vs_fn;
            pDesc.fragmentFunction = ps_text_fn;
            pDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            pDesc.colorAttachments[0].blendingEnabled = YES;
            pDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

            pipeline_ui_text = [device newRenderPipelineStateWithDescriptor:pDesc error:&err];
            if (err || !pipeline_ui_text) {
                DC_ERROR("metal_renderer: failed to create pipeline_ui_text: %s",
                         [[err localizedDescription] UTF8String]);
            } else {
                DC_INFO("metal_renderer: pipeline_ui_text initialized successfully");
            }
        }
    }

    id<MTLTexture> GetOrCreateGuestTexture(const Liverpool::UserData& ud) {
        const u32 w0 = ud[4];
        const u32 w1 = ud[5];
        const u32 w2 = ud[6];
        const u32 w3 = ud[7];
        if (!w0 && !w1) {
            return default_white_texture;
        }

        // Validate image descriptor type if present (bits 28..31 in word 3)
        if (w3 != 0 && ((w3 >> 28) & 0x8) == 0) {
            return default_white_texture;
        }

        GuestAddr addr = static_cast<GuestAddr>(w0) << 8;
        if (addr < 0x400000) {
            return default_white_texture;
        }
        if (addr < 0x100000000ULL) {
            const VAddr va = Core::Memory::PhysToVirt(addr);
            if (va != 0) {
                addr = va;
            }
        }

        auto it = guest_textures.find(addr);
        if (it != guest_textures.end() && it->second) {
            return it->second;
        }

        const u32 width = (w2 & 0x3FFF) + 1;
        const u32 height = ((w2 >> 14) & 0x3FFF) + 1;
        if (width > 4096 || height > 4096 || width == 0 || height == 0) {
            return default_white_texture;
        }
        const u32 dfmt = (w1 >> 20) & 0x3F;

        MTLPixelFormat pix_fmt = MTLPixelFormatR8Unorm;
        NSUInteger bpp = 1;
        if (dfmt == 10) {
            pix_fmt = MTLPixelFormatRGBA8Unorm;
            bpp = 4;
        }

        Core::Memory::Vma vma{};
        const u64 total_bytes = static_cast<u64>(width) * bpp * height;
        if (!Core::Memory::Query(addr, false, &vma) || (addr + total_bytes > vma.start + vma.size)) {
            DC_WARN("metal_renderer: guest texture addr %#llx (+%#llx bytes) not mapped! Defaulting to white",
                    addr, total_bytes);
            guest_textures[addr] = default_white_texture;
            return default_white_texture;
        }

        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:pix_fmt
                                         width:width
                                        height:height
                                     mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;

        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        if (tex) {
            [tex replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:reinterpret_cast<const void*>(addr)
                   bytesPerRow:width * bpp];
        }
        guest_textures[addr] = tex ? tex : default_white_texture;
        DC_INFO("metal_renderer: created guest texture %ux%u dfmt=%u at addr %#llx", width, height, dfmt, addr);
        return guest_textures[addr];
    }

    id<MTLTexture> GetOrCreateTexture(GuestAddr addr, u32 width, u32 height) {
        if (!addr || width == 0 || height == 0) {
            return nil;
        }
        auto it = target_textures.find(addr);
        if (it != target_textures.end()) {
            if (it->second.width == width && it->second.height == height) {
                return it->second;
            }
        }

        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:width
            height:height
            mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        desc.storageMode = MTLStorageModeShared;

        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        target_textures[addr] = tex;
        DC_INFO("metal_renderer: created render target %ux%u for addr %#llx", width, height, addr);
        return tex;
    }

    void EnsureCommandBuffer() {
        if (!current_cmd_buf) {
            current_cmd_buf = [queue commandBuffer];
        }
    }

    void EndCurrentEncoder() {
        if (current_encoder) {
            [current_encoder endEncoding];
            current_encoder = nil;
            current_render_target = nil;
            current_target_addr = 0;
        }
    }

    void CommitCommandBuffer() {
        EndCurrentEncoder();
        if (current_cmd_buf) {
            [current_cmd_buf commit];
            [current_cmd_buf waitUntilCompleted];
            current_cmd_buf = nil;
        }
    }
};

MetalRenderer::MetalRenderer(Device& device)
    : impl(std::make_unique<Impl>()) {
    impl->device = (__bridge id<MTLDevice>)device.Handle();
    impl->queue = (__bridge id<MTLCommandQueue>)device.Queue();
    impl->InitPipelines();
    DC_INFO("metal_renderer: initialized with device %s", device.Info().name.c_str());
}

MetalRenderer::~MetalRenderer() {
    impl->CommitCommandBuffer();
}

void MetalRenderer::Draw(const Liverpool::Regs& regs, bool indexed) {
    static u64 draw_counter = 0;
    ++draw_counter;

    const auto& cb0 = regs.color_buffers[0];
    const GuestAddr cb_addr = cb0.Address();
    if (!cb_addr) {
        return;
    }

    const u32 width = cb0.Pitch();
    const u32 height = cb0.Height() > 0 ? cb0.Height() : 1080;
    if (width == 0 || height == 0) {
        return;
    }

    if (draw_counter <= 10 || draw_counter % 200 == 0) {
        const u64 vs_addr = regs.vs_program.Address<uintptr_t>();
        const u64 ps_addr = regs.ps_program.Address<uintptr_t>();
        const u32* vs_ptr = reinterpret_cast<const u32*>(vs_addr);
        const u32* ps_ptr = reinterpret_cast<const u32*>(ps_addr);
        u32 vs_w0 = vs_ptr ? vs_ptr[0] : 0;
        u32 vs_w1 = vs_ptr ? vs_ptr[1] : 0;
        u32 ps_w0 = ps_ptr ? ps_ptr[0] : 0;
        u32 ps_w1 = ps_ptr ? ps_ptr[1] : 0;
        DC_INFO("metal_renderer: draw #%llu prim=%u indices=%u indexed=%d cb0=%#llx (%ux%u) vs=%#llx [%#x, %#x] ps=%#llx [%#x, %#x]",
                draw_counter, static_cast<u32>(regs.primitive_type), regs.num_indices, indexed ? 1 : 0,
                cb_addr, width, height, vs_addr, vs_w0, vs_w1, ps_addr, ps_w0, ps_w1);
        DC_INFO("metal_renderer: vs user_data[0..7]: %#x %#x %#x %#x | %#x %#x %#x %#x",
                regs.vs_program.user_data[0], regs.vs_program.user_data[1],
                regs.vs_program.user_data[2], regs.vs_program.user_data[3],
                regs.vs_program.user_data[4], regs.vs_program.user_data[5],
                regs.vs_program.user_data[6], regs.vs_program.user_data[7]);
    }

    if (draw_counter == 5) {
        DC_INFO("metal_renderer: draw #5 vs_output_config: num_exports=%u, num_interp=%u, color_export_format=%#x",
                regs.vs_output_config.NumExports(), regs.num_interp, regs.color_export_format.raw);
        DC_INFO("metal_renderer: draw #5 blend_control[0]: color_func=%u, alpha_func=%u, enable=%u",
                static_cast<u32>(regs.blend_control[0].color_func),
                static_cast<u32>(regs.blend_control[0].alpha_func),
                regs.blend_control[0].enable);
        FILE* fp = fopen("/tmp/vs_ui.bin", "wb");
        if (fp) {
            fwrite(reinterpret_cast<const void*>(regs.vs_program.Address<uintptr_t>()), 1, 2048, fp);
            fclose(fp);
        }
        fp = fopen(draw_counter == 5 ? "/tmp/ps_ui.bin" : "/tmp/ps_ui_text.bin", "wb");
        if (fp) {
            fwrite(reinterpret_cast<const void*>(regs.ps_program.Address<uintptr_t>()), 1, 2048, fp);
            fclose(fp);
        }
        fp = fopen("/tmp/vbuf.bin", "wb");
        if (fp) {
            fwrite(reinterpret_cast<const void*>(0x3000d01b0ULL), 1, 2048, fp);
            fclose(fp);
        }
    }

    if (draw_counter == 8) {
        const GuestAddr vb = ((uint64_t(regs.vs_program.user_data[5] & 0xFFFF) << 32) | regs.vs_program.user_data[4]);
        const GuestAddr cb = ((uint64_t(regs.vs_program.user_data[1] & 0xFFFF) << 32) | regs.vs_program.user_data[0]);
        DC_INFO("metal_renderer: draw #8 vb=%#llx cb=%#llx num_indices=%u", vb, cb, regs.num_indices);
        FILE* fp = fopen("/tmp/vbuf_draw8.bin", "wb");
        if (fp && vb) {
            fwrite(reinterpret_cast<const void*>(vb), 1, regs.num_indices * 16, fp);
            fclose(fp);
        }
        fp = fopen("/tmp/cbuf_draw8.bin", "wb");
        if (fp && cb) {
            fwrite(reinterpret_cast<const void*>(cb), 1, 512, fp);
            fclose(fp);
        }
    }

    // Handle EliminateFastClear:
    // In AMD GCN hardware, EliminateFastClear is an end-of-frame resolve of
    // delta color compression metadata (DCC/CMask) before scanout.
    // In Metal, compression is handled automatically and clearing here wipes out rendered UI!
    if (regs.color_control.mode == Liverpool::ColorControl::OperationMode::EliminateFastClear) {
        return;
    }

    id<MTLTexture> tex = impl->GetOrCreateTexture(cb_addr, width, height);
    if (!tex) {
        return;
    }

    impl->EnsureCommandBuffer();

    if (impl->current_render_target != tex) {
        impl->EndCurrentEncoder();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = tex;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        impl->current_encoder = [impl->current_cmd_buf renderCommandEncoderWithDescriptor:pass];
        impl->current_render_target = tex;
        impl->current_target_addr = cb_addr;
    }

    // Set viewport from regs.viewports[0]
    const auto& vp = regs.viewports[0];
    if (vp.xscale != 0.0f || vp.yscale != 0.0f) {
        const double vp_x = std::max(0.0, static_cast<double>(vp.xoffset - std::abs(vp.xscale)));
        const double vp_y = std::max(0.0, static_cast<double>(vp.yoffset - std::abs(vp.yscale)));
        const double vp_w = std::abs(vp.xscale) * 2.0;
        const double vp_h = std::abs(vp.yscale) * 2.0;
        if (vp_w > 0.0 && vp_h > 0.0) {
            MTLViewport mtl_vp{
                .originX = vp_x,
                .originY = vp_y,
                .width = vp_w,
                .height = vp_h,
                .znear = static_cast<double>(vp.zoffset - vp.zscale),
                .zfar = static_cast<double>(vp.zoffset + vp.zscale),
            };
            [impl->current_encoder setViewport:mtl_vp];
        }
    }

    // Set scissor
    const auto& sc = regs.screen_scissor;
    const u32 sc_w = sc.GetWidth();
    const u32 sc_h = sc.GetHeight();
    if (sc_w > 0 && sc_h > 0 && sc_w <= width && sc_h <= height) {
        MTLScissorRect mtl_sc{
            .x = static_cast<NSUInteger>(std::max(s16(0), sc.top_left_x)),
            .y = static_cast<NSUInteger>(std::max(s16(0), sc.top_left_y)),
            .width = static_cast<NSUInteger>(sc_w),
            .height = static_cast<NSUInteger>(sc_h),
        };
        [impl->current_encoder setScissorRect:mtl_sc];
    }

    // Execute QuadList UI draws
    if (regs.primitive_type == Liverpool::PrimitiveType::QuadList && regs.num_indices >= 4) {
        const u64 vs_addr = regs.vs_program.Address<uintptr_t>();
        const u64 ps_addr = regs.ps_program.Address<uintptr_t>();
        const u32* vs_ptr = reinterpret_cast<const u32*>(vs_addr);
        const u32* ps_ptr = reinterpret_cast<const u32*>(ps_addr);
        const u32 vs_w0 = vs_ptr ? vs_ptr[0] : 0;
        const u32 ps_w1 = ps_ptr ? ps_ptr[1] : 0;

        if (vs_w0 == 0xbeeb03ff) {
            id<MTLRenderPipelineState> pipeline = impl->pipeline_ui_panel;
            if (ps_addr == 0x403572900 || ps_w1 == 0x6e) {
                pipeline = impl->pipeline_ui_text;
            }

            if (pipeline) {
                [impl->current_encoder setRenderPipelineState:pipeline];

                const GuestAddr vb_addr = ((uint64_t(regs.vs_program.user_data[5] & 0xFFFF) << 32) | regs.vs_program.user_data[4]);
                const GuestAddr cb_addr = ((uint64_t(regs.vs_program.user_data[1] & 0xFFFF) << 32) | regs.vs_program.user_data[0]);

                Core::Memory::Vma vma_b{};
                if (cb_addr >= 0x400000 && Core::Memory::Query(cb_addr, false, &vma_b) && cb_addr + 512 <= vma_b.start + vma_b.size) {
                    [impl->current_encoder setVertexBytes:reinterpret_cast<const void*>(cb_addr)
                                                   length:512
                                                  atIndex:0];
                }
                const u32 num_quads = regs.num_indices / 4;
                const size_t vb_size = std::min<size_t>(1024 * 1024, std::max<size_t>(num_quads * 64, 4096));
                if (vb_addr >= 0x400000 && Core::Memory::Query(vb_addr, false, &vma_b) && vb_addr + vb_size <= vma_b.start + vma_b.size) {
                    std::memcpy([impl->dynamic_vbuf contents], reinterpret_cast<const void*>(vb_addr), vb_size);
                    [impl->current_encoder setVertexBuffer:impl->dynamic_vbuf offset:0 atIndex:1];
                    [impl->current_encoder setVertexBuffer:impl->dynamic_vbuf offset:0 atIndex:2];
                }

                const GuestAddr ps_cb_addr = (0x300000000ULL | regs.ps_program.user_data[0]);
                if (ps_cb_addr >= 0x400000 && Core::Memory::Query(ps_cb_addr, false, &vma_b) && ps_cb_addr + 2048 <= vma_b.start + vma_b.size) {
                    [impl->current_encoder setFragmentBytes:reinterpret_cast<const void*>(ps_cb_addr)
                                                     length:2048
                                                    atIndex:0];
                    [impl->current_encoder setFragmentBytes:reinterpret_cast<const void*>(ps_cb_addr)
                                                     length:2048
                                                    atIndex:1];
                }

                id<MTLTexture> font_tex = impl->GetOrCreateGuestTexture(regs.ps_program.user_data);
                [impl->current_encoder setFragmentTexture:font_tex ? font_tex : impl->default_white_texture
                                                  atIndex:0];
                [impl->current_encoder setFragmentSamplerState:impl->default_sampler atIndex:0];

                const u32 index_count = num_quads * 6;
                [impl->current_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                                  indexCount:index_count
                                                   indexType:MTLIndexTypeUInt16
                                                 indexBuffer:impl->quad_index_buffer
                                           indexBufferOffset:0];

                impl->last_drawn_target = tex;

                if (draw_counter <= 15 || draw_counter % 200 == 0) {
                    DC_INFO("metal_renderer: EXECUTED UI DRAW #%llu: quads=%u triangles=%u vs=%#llx ps=%#llx",
                            draw_counter, num_quads, num_quads * 2, vs_addr, ps_addr);
                }
            }
        }
    }
}

void MetalRenderer::DrawIndirect(const Liverpool::Regs& regs, bool indexed, GuestAddr args,
                                 std::uint32_t draw_count, GuestAddr count_addr) {
    (void)regs;
    (void)indexed;
    (void)args;
    (void)draw_count;
    (void)count_addr;
}

void MetalRenderer::Dispatch(const Liverpool::Regs& regs, std::uint32_t x, std::uint32_t y,
                             std::uint32_t z) {
    (void)regs;
    (void)x;
    (void)y;
    (void)z;
}

void MetalRenderer::DispatchIndirect(const Liverpool::Regs& regs, GuestAddr args) {
    (void)regs;
    (void)args;
}

void MetalRenderer::CopyBuffer(GuestAddr dst, GuestAddr src, std::uint64_t size) {
    if (dst && src && size > 0) {
        std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), size);
    }
}

void MetalRenderer::FillBuffer(GuestAddr dst, std::uint32_t value, std::uint64_t size) {
    if (dst && size > 0) {
        std::memset(reinterpret_cast<void*>(dst), static_cast<int>(value), size);
    }
}

void MetalRenderer::InvalidateMemory(GuestAddr addr, std::uint64_t size) {
    (void)addr;
    (void)size;
}

void MetalRenderer::FlushMemory(GuestAddr addr, std::uint64_t size) {
    (void)addr;
    (void)size;
}

void MetalRenderer::OnSubmit() {
    impl->CommitCommandBuffer();
}

void MetalRenderer::SignalFence(GuestAddr addr, std::uint64_t value, bool interrupt) {
    if (addr) {
        *reinterpret_cast<volatile u64*>(addr) = value;
    }
    (void)interrupt;
}

void MetalRenderer::Flip(GuestAddr framebuffer, std::uint32_t width, std::uint32_t height,
                         std::uint32_t pitch, std::uint32_t format) {
    impl->CommitCommandBuffer();

    id<MTLTexture> src_tex = nil;
    if (impl->last_drawn_target) {
        src_tex = impl->last_drawn_target;
    }
    if (!src_tex) {
        auto it = impl->target_textures.find(framebuffer);
        if (it != impl->target_textures.end() && it->second) {
            src_tex = it->second;
        }
    }

    static u64 flip_count = 0;
    ++flip_count;

    if (src_tex) {
        {
            std::scoped_lock lk(impl->latest_tex_mutex);
            impl->latest_texture = src_tex;
        }

        if (flip_count <= 5 || flip_count % 60 == 0) {
            DC_INFO("metal_renderer: Flip #%llu: texture=%p (%ux%u) for fb=%#llx",
                    flip_count, (__bridge void*)src_tex, width, height, framebuffer);
        }

        if (flip_count == 3 || flip_count == 10) {
            id<MTLBuffer> sbuf = [impl->device newBufferWithLength:width * height * 4
                                                           options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> scmd = [impl->queue commandBuffer];
            id<MTLBlitCommandEncoder> sblit = [scmd blitCommandEncoder];
            [sblit copyFromTexture:src_tex
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(width, height, 1)
                          toBuffer:sbuf
                 destinationOffset:0
            destinationBytesPerRow:width * 4
          destinationBytesPerImage:width * height * 4];
            [sblit endEncoding];
            [scmd commit];
            [scmd waitUntilCompleted];

            const uint8_t* p = reinterpret_cast<const uint8_t*>([sbuf contents]);
            char path[64];
            snprintf(path, sizeof(path), "/tmp/screen_%llu.ppm", flip_count);
            FILE* f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", width, height);
                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        uint32_t idx = y * (width * 4) + x * 4;
                        // BGRA -> RGB
                        uint8_t rgb[3] = {p[idx + 2], p[idx + 1], p[idx + 0]};
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
                DC_INFO("metal_renderer: saved hardware GPU screenshot to %s", path);
            }
        }
    } else {
        if (flip_count <= 5 || flip_count % 60 == 0) {
            DC_WARN("metal_renderer: Flip #%llu skipped: src_tex=%p fb=%#llx",
                    flip_count, (__bridge void*)src_tex, framebuffer);
        }
    }
    (void)pitch;
    (void)format;
}

void* MetalRenderer::GetLatestTexture() const {
    std::scoped_lock lk(impl->latest_tex_mutex);
    return (__bridge void*)impl->latest_texture;
}

void MetalRenderer::PushMarker(const char* name) {
    (void)name;
}

void MetalRenderer::PopMarker() {
}

} // namespace Gpu::Metal
