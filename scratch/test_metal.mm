#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <iostream>
#include <vector>

int main() {
    id<MTLDevice> dev = nil;
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    for (id<MTLDevice> d in devices) {
        if (![d isLowPower]) {
            dev = d;
            break;
        }
    }
    if (!dev) dev = MTLCreateSystemDefaultDevice();
    std::cout << "Using device: " << [[dev name] UTF8String] << std::endl;
    id<MTLCommandQueue> queue = [dev newCommandQueue];

    // Method 1: MTLStorageModeManaged with blit synchronizeTexture
    {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:64
            height:64
            mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeManaged;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:desc];

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = tex;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.5, 0.25, 1.0);

        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];

        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit synchronizeTexture:tex slice:0 level:0];
        [blit endEncoding];

        [cmd commit];
        [cmd waitUntilCompleted];

        std::vector<uint32_t> pixels(64 * 64, 0);
        [tex getBytes:pixels.data()
          bytesPerRow:64 * 4
           fromRegion:MTLRegionMake2D(0, 0, 64, 64)
          mipmapLevel:0];

        int non_zero = 0;
        for (uint32_t p : pixels) if (p != 0) non_zero++;
        std::cout << "Method 1 (Managed + synchronizeTexture): non_zero=" << non_zero
                  << " / 4096, sample[0]=0x" << std::hex << pixels[0] << std::dec << std::endl;
    }

    // Method 2: Copy to MTLBuffer
    {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:64
            height:64
            mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:desc];

        id<MTLBuffer> buf = [dev newBufferWithLength:64 * 64 * 4 options:MTLResourceStorageModeShared];

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = tex;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.5, 0.25, 1.0);

        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];

        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit copyFromTexture:tex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(64, 64, 1)
                     toBuffer:buf
            destinationOffset:0
       destinationBytesPerRow:64 * 4
     destinationBytesPerImage:64 * 64 * 4];
        [blit endEncoding];

        [cmd commit];
        [cmd waitUntilCompleted];

        const uint32_t* ptr = (const uint32_t*)[buf contents];
        int non_zero = 0;
        for (int i = 0; i < 64 * 64; ++i) if (ptr[i] != 0) non_zero++;
        std::cout << "Method 2 (Private + copyFromTexture to Shared MTLBuffer): non_zero=" << non_zero
                  << " / 4096, sample[0]=0x" << std::hex << ptr[0] << std::dec << std::endl;
    }

    return 0;
}
