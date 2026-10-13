// SPDX-License-Identifier: GPL-2.0-or-later

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <iostream>
#include <fstream>
#include <sstream>

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::cerr << "No Metal device!\n";
            return 1;
        }
        std::cout << "Metal device: " << [[device name] UTF8String] << "\n";

        std::ifstream vs_f("/tmp/vs_ui.metal");
        std::stringstream vs_ss;
        vs_ss << vs_f.rdbuf();
        NSString* vs_src = [NSString stringWithUTF8String:vs_ss.str().c_str()];

        std::ifstream ps_f("/tmp/ps_ui.metal");
        std::stringstream ps_ss;
        ps_ss << ps_f.rdbuf();
        NSString* ps_src = [NSString stringWithUTF8String:ps_ss.str().c_str()];

        NSError* err = nil;
        id<MTLLibrary> vs_lib = [device newLibraryWithSource:vs_src options:nil error:&err];
        if (!vs_lib) {
            std::cerr << "VS compile error: " << [[err localizedDescription] UTF8String] << "\n";
            return 1;
        }

        id<MTLLibrary> ps_lib = [device newLibraryWithSource:ps_src options:nil error:&err];
        if (!ps_lib) {
            std::cerr << "PS compile error: " << [[err localizedDescription] UTF8String] << "\n";
            return 1;
        }

        id<MTLFunction> vs_fn = [vs_lib newFunctionWithName:@"main0"];
        id<MTLFunction> ps_fn = [ps_lib newFunctionWithName:@"main0"];

        MTLRenderPipelineDescriptor* pDesc = [MTLRenderPipelineDescriptor new];
        pDesc.vertexFunction = vs_fn;
        pDesc.fragmentFunction = ps_fn;
        pDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pDesc.colorAttachments[0].blendingEnabled = YES;
        pDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;

        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pDesc error:&err];
        if (!pipeline) {
            std::cerr << "Pipeline error: " << [[err localizedDescription] UTF8String] << "\n";
            return 1;
        }

        std::cout << "SUCCESS! Pipeline created: " << pipeline << "\n";
    }
    return 0;
}
