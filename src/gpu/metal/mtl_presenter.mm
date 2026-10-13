#include "gpu/metal/mtl_presenter.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu/metal/mtl_device.h"

namespace Gpu::Metal {

struct Presenter::Impl {
    id<MTLCommandQueue> queue;
    CAMetalLayer* layer;
    dispatch_semaphore_t frames_in_flight;
};

Presenter::Presenter(Device& device, void* layer, const PresenterConfig& config)
    : impl(std::make_unique<Impl>()) {
    impl->queue = (__bridge id<MTLCommandQueue>)device.Queue();
    impl->layer = (__bridge CAMetalLayer*)layer;
    impl->frames_in_flight = dispatch_semaphore_create(kFramesInFlight);

    CAMetalLayer* l = impl->layer;
    l.device = (__bridge id<MTLDevice>)device.Handle();
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // Only ever rendered to, never sampled: lets the driver skip extra copies.
    l.framebufferOnly = YES;
    l.maximumDrawableCount = kFramesInFlight;
    l.displaySyncEnabled = config.vsync;
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

        // One retained command buffer per frame is negligible; the heavy guest
        // command buffers will use unretained references instead.
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

} // namespace Gpu::Metal
