#include "gpu/metal/mtl_presenter.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu/metal/mtl_device.h"

namespace Gpu::Metal {

namespace {

constexpr const char* kPresentShaderSrc = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vs_present(uint vid [[vertex_id]]) {
    VertexOut out;
    out.uv = float2((vid << 1) & 2, vid & 2);
    out.position = float4(out.uv.x * 2.0 - 1.0, 1.0 - out.uv.y * 2.0, 0.0, 1.0);
    return out;
}

fragment float4 fs_present(VertexOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv);
}
)";

} // namespace

struct Presenter::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    CAMetalLayer* layer;
    id<MTLRenderPipelineState> pipeline;
    id<MTLSamplerState> sampler;
    dispatch_semaphore_t frames_in_flight;
};

Presenter::Presenter(Device& device, void* layer, const PresenterConfig& config)
    : impl(std::make_unique<Impl>()) {
    impl->device = (__bridge id<MTLDevice>)device.Handle();
    impl->queue = (__bridge id<MTLCommandQueue>)device.Queue();
    impl->layer = (__bridge CAMetalLayer*)layer;
    impl->frames_in_flight = dispatch_semaphore_create(kFramesInFlight);

    CAMetalLayer* l = impl->layer;
    l.device = impl->device;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // Only ever rendered to, never sampled: lets the driver skip extra copies.
    l.framebufferOnly = YES;
    l.maximumDrawableCount = kFramesInFlight;
    l.displaySyncEnabled = config.vsync;

    // Create fullscreen present pipeline
    NSError* err = nil;
    NSString* src = [NSString stringWithUTF8String:kPresentShaderSrc];
    id<MTLLibrary> lib = [impl->device newLibraryWithSource:src options:nil error:&err];
    if (err) {
        NSLog(@"Presenter shader compilation failed: %@", err);
    }
    id<MTLFunction> vs = [lib newFunctionWithName:@"vs_present"];
    id<MTLFunction> fs = [lib newFunctionWithName:@"fs_present"];

    MTLRenderPipelineDescriptor* pDesc = [MTLRenderPipelineDescriptor new];
    pDesc.vertexFunction = vs;
    pDesc.fragmentFunction = fs;
    pDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    impl->pipeline = [impl->device newRenderPipelineStateWithDescriptor:pDesc error:&err];
    if (err) {
        NSLog(@"Presenter pipeline creation failed: %@", err);
    }

    MTLSamplerDescriptor* sDesc = [MTLSamplerDescriptor new];
    sDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sDesc.magFilter = MTLSamplerMinMagFilterLinear;
    impl->sampler = [impl->device newSamplerStateWithDescriptor:sDesc];
}

Presenter::~Presenter() {
    // Drain in-flight frames so their completion handlers don't outlive us.
    for (int i = 0; i < kFramesInFlight; ++i) {
        dispatch_semaphore_wait(impl->frames_in_flight, DISPATCH_TIME_FOREVER);
    }
    // libdispatch traps if a semaphore dies below its initial value.
    for (int i = 0; i < kFramesInFlight; ++i) {
        dispatch_semaphore_signal(impl->frames_in_flight);
    }
}

void Presenter::Resize(int width_px, int height_px) {
    impl->layer.drawableSize = CGSizeMake(width_px, height_px);
}

void Presenter::SetVsync(bool enabled) {
    impl->layer.displaySyncEnabled = enabled;
}

void Presenter::PresentClear(float r, float g, float b) {
    dispatch_semaphore_wait(impl->frames_in_flight, DISPATCH_TIME_FOREVER);

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [impl->layer nextDrawable];
        if (!drawable) {
            dispatch_semaphore_signal(impl->frames_in_flight);
            return;
        }

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1.0);

        id<MTLCommandBuffer> cmd = [impl->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];

        dispatch_semaphore_t sem = impl->frames_in_flight;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
          dispatch_semaphore_signal(sem);
        }];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
}

void Presenter::PresentTexture(void* texture_handle) {
    if (!texture_handle || !impl->pipeline) {
        return;
    }
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture_handle;

    dispatch_semaphore_wait(impl->frames_in_flight, DISPATCH_TIME_FOREVER);

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [impl->layer nextDrawable];
        if (!drawable) {
            dispatch_semaphore_signal(impl->frames_in_flight);
            return;
        }

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cmd = [impl->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        [enc setRenderPipelineState:impl->pipeline];
        [enc setFragmentTexture:tex atIndex:0];
        [enc setFragmentSamplerState:impl->sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];

        dispatch_semaphore_t sem = impl->frames_in_flight;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
          dispatch_semaphore_signal(sem);
        }];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
}

} // namespace Gpu::Metal
