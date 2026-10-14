#pragma once

// Guest (CB / T# / DB) formats and render state -> Metal equivalents.
// Objective-C++ only.

#import <Metal/Metal.h>

#include "common/types.h"
#include "gpu/liverpool/registers.h"
#include "gpu/shader/msl.h"

namespace Gpu::Metal {

struct ColorFormatInfo {
    MTLPixelFormat format = MTLPixelFormatInvalid;
    Shader::OutType out = Shader::OutType::Float;
};

// CB_COLORn_INFO FORMAT + NUMBER_TYPE.
inline ColorFormatInfo ColorTargetFormat(u32 cb_format, u32 number_type) {
    using namespace Liverpool;
    using Shader::OutType;
    const bool srgb = number_type == ColorInfo::kNumberSrgb;
    const bool uint = number_type == NumFormat::Uint;
    const bool sint = number_type == NumFormat::Sint;
    const bool snorm = number_type == NumFormat::Snorm;
    const bool flt = number_type == NumFormat::Float;
    const OutType out = uint ? OutType::Uint : sint ? OutType::Sint : OutType::Float;
    MTLPixelFormat f = MTLPixelFormatInvalid;
    switch (cb_format) {
    case ColorFormat::C8:
        f = uint ? MTLPixelFormatR8Uint : sint ? MTLPixelFormatR8Sint : snorm ? MTLPixelFormatR8Snorm : MTLPixelFormatR8Unorm;
        break;
    case ColorFormat::C8_8:
        f = uint ? MTLPixelFormatRG8Uint : sint ? MTLPixelFormatRG8Sint : snorm ? MTLPixelFormatRG8Snorm : MTLPixelFormatRG8Unorm;
        break;
    case ColorFormat::C8_8_8_8:
        f = srgb ? MTLPixelFormatRGBA8Unorm_sRGB
            : uint ? MTLPixelFormatRGBA8Uint
            : sint ? MTLPixelFormatRGBA8Sint
            : snorm ? MTLPixelFormatRGBA8Snorm : MTLPixelFormatRGBA8Unorm;
        break;
    case ColorFormat::C16:
        f = flt ? MTLPixelFormatR16Float : uint ? MTLPixelFormatR16Uint : sint ? MTLPixelFormatR16Sint
            : snorm ? MTLPixelFormatR16Snorm : MTLPixelFormatR16Unorm;
        break;
    case ColorFormat::C16_16:
        f = flt ? MTLPixelFormatRG16Float : uint ? MTLPixelFormatRG16Uint : sint ? MTLPixelFormatRG16Sint
            : snorm ? MTLPixelFormatRG16Snorm : MTLPixelFormatRG16Unorm;
        break;
    case ColorFormat::C16_16_16_16:
        f = flt ? MTLPixelFormatRGBA16Float : uint ? MTLPixelFormatRGBA16Uint : sint ? MTLPixelFormatRGBA16Sint
            : snorm ? MTLPixelFormatRGBA16Snorm : MTLPixelFormatRGBA16Unorm;
        break;
    case ColorFormat::C32:
        f = uint ? MTLPixelFormatR32Uint : sint ? MTLPixelFormatR32Sint : MTLPixelFormatR32Float;
        break;
    case ColorFormat::C32_32:
        f = uint ? MTLPixelFormatRG32Uint : sint ? MTLPixelFormatRG32Sint : MTLPixelFormatRG32Float;
        break;
    case ColorFormat::C32_32_32_32:
        f = uint ? MTLPixelFormatRGBA32Uint : sint ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
        break;
    case ColorFormat::C10_11_11: // R11 G11 B10, red in the low bits
    case ColorFormat::C11_11_10: // R10 G11 B11: no Metal format, but targets stay on the GPU
        f = MTLPixelFormatRG11B10Float;
        break;
    case ColorFormat::C2_10_10_10:
        f = uint ? MTLPixelFormatRGB10A2Uint : MTLPixelFormatRGB10A2Unorm;
        break;
    default:
        // Packed 16-bit and other rare formats: render at higher precision.
        f = MTLPixelFormatRGBA8Unorm;
        break;
    }
    return {f, out};
}

struct TextureFormatInfo {
    MTLPixelFormat format = MTLPixelFormatInvalid;
    u32 bytes_per_block = 0; // per texel, or per 4x4 block when compressed
    bool compressed = false;
    Shader::TexSample sample = Shader::TexSample::Float;
};

// T# DATA_FORMAT + NUM_FORMAT. Invalid when we cannot upload it directly.
inline TextureFormatInfo TextureFormat(u32 dfmt, u32 nfmt) {
    using namespace Liverpool;
    using Shader::TexSample;
    const bool srgb = nfmt == NumFormat::Srgb;
    const bool uint = nfmt == NumFormat::Uint;
    const bool sint = nfmt == NumFormat::Sint;
    const bool snorm = nfmt == NumFormat::Snorm;
    const bool flt = nfmt == NumFormat::Float;
    const TexSample s = uint ? TexSample::Uint : sint ? TexSample::Sint : TexSample::Float;
    TextureFormatInfo t{MTLPixelFormatInvalid, 0, false, s};
    switch (dfmt) {
    case 1:
        t.format = uint ? MTLPixelFormatR8Uint : sint ? MTLPixelFormatR8Sint : snorm ? MTLPixelFormatR8Snorm : MTLPixelFormatR8Unorm;
        t.bytes_per_block = 1;
        break;
    case 2:
        t.format = flt ? MTLPixelFormatR16Float : uint ? MTLPixelFormatR16Uint : sint ? MTLPixelFormatR16Sint
                 : snorm ? MTLPixelFormatR16Snorm : MTLPixelFormatR16Unorm;
        t.bytes_per_block = 2;
        break;
    case 3:
        t.format = uint ? MTLPixelFormatRG8Uint : sint ? MTLPixelFormatRG8Sint : snorm ? MTLPixelFormatRG8Snorm : MTLPixelFormatRG8Unorm;
        t.bytes_per_block = 2;
        break;
    case 4:
        t.format = uint ? MTLPixelFormatR32Uint : sint ? MTLPixelFormatR32Sint : MTLPixelFormatR32Float;
        t.bytes_per_block = 4;
        break;
    case 5:
        t.format = flt ? MTLPixelFormatRG16Float : uint ? MTLPixelFormatRG16Uint : sint ? MTLPixelFormatRG16Sint
                 : snorm ? MTLPixelFormatRG16Snorm : MTLPixelFormatRG16Unorm;
        t.bytes_per_block = 4;
        break;
    case 6:
        t.format = MTLPixelFormatRG11B10Float;
        t.bytes_per_block = 4;
        break;
    case 9:
        t.format = uint ? MTLPixelFormatRGB10A2Uint : MTLPixelFormatRGB10A2Unorm;
        t.bytes_per_block = 4;
        break;
    case 10:
        t.format = srgb ? MTLPixelFormatRGBA8Unorm_sRGB : uint ? MTLPixelFormatRGBA8Uint : sint ? MTLPixelFormatRGBA8Sint
                 : snorm ? MTLPixelFormatRGBA8Snorm : MTLPixelFormatRGBA8Unorm;
        t.bytes_per_block = 4;
        break;
    case 11:
        t.format = uint ? MTLPixelFormatRG32Uint : sint ? MTLPixelFormatRG32Sint : MTLPixelFormatRG32Float;
        t.bytes_per_block = 8;
        break;
    case 12:
        t.format = flt ? MTLPixelFormatRGBA16Float : uint ? MTLPixelFormatRGBA16Uint : sint ? MTLPixelFormatRGBA16Sint
                 : snorm ? MTLPixelFormatRGBA16Snorm : MTLPixelFormatRGBA16Unorm;
        t.bytes_per_block = 8;
        break;
    case 14:
        t.format = uint ? MTLPixelFormatRGBA32Uint : sint ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
        t.bytes_per_block = 16;
        break;
    case 34:
        t.format = MTLPixelFormatRGB9E5Float;
        t.bytes_per_block = 4;
        break;
    case 35:
        t.format = srgb ? MTLPixelFormatBC1_RGBA_sRGB : MTLPixelFormatBC1_RGBA;
        t.bytes_per_block = 8;
        t.compressed = true;
        break;
    case 36:
        t.format = srgb ? MTLPixelFormatBC2_RGBA_sRGB : MTLPixelFormatBC2_RGBA;
        t.bytes_per_block = 16;
        t.compressed = true;
        break;
    case 37:
        t.format = srgb ? MTLPixelFormatBC3_RGBA_sRGB : MTLPixelFormatBC3_RGBA;
        t.bytes_per_block = 16;
        t.compressed = true;
        break;
    case 38:
        t.format = snorm ? MTLPixelFormatBC4_RSnorm : MTLPixelFormatBC4_RUnorm;
        t.bytes_per_block = 8;
        t.compressed = true;
        break;
    case 39:
        t.format = snorm ? MTLPixelFormatBC5_RGSnorm : MTLPixelFormatBC5_RGUnorm;
        t.bytes_per_block = 16;
        t.compressed = true;
        break;
    case 40:
        t.format = snorm ? MTLPixelFormatBC6H_RGBFloat : MTLPixelFormatBC6H_RGBUfloat;
        t.bytes_per_block = 16;
        t.compressed = true;
        break;
    case 41:
        t.format = srgb ? MTLPixelFormatBC7_RGBAUnorm_sRGB : MTLPixelFormatBC7_RGBAUnorm;
        t.bytes_per_block = 16;
        t.compressed = true;
        break;
    default:
        break;
    }
    if (t.sample != TexSample::Float && t.compressed) {
        t.sample = TexSample::Float;
    }
    return t;
}

inline MTLTextureSwizzle Swizzle(u32 sel) {
    switch (sel & 7) {
    case 0: return MTLTextureSwizzleZero;
    case 1: return MTLTextureSwizzleOne;
    case 4: return MTLTextureSwizzleRed;
    case 5: return MTLTextureSwizzleGreen;
    case 6: return MTLTextureSwizzleBlue;
    case 7: return MTLTextureSwizzleAlpha;
    default: return MTLTextureSwizzleZero;
    }
}

inline MTLBlendFactor BlendFactor(u32 f) {
    switch (f) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorOne;
    case 2: return MTLBlendFactorSourceColor;
    case 3: return MTLBlendFactorOneMinusSourceColor;
    case 4: return MTLBlendFactorSourceAlpha;
    case 5: return MTLBlendFactorOneMinusSourceAlpha;
    case 6: return MTLBlendFactorDestinationAlpha;
    case 7: return MTLBlendFactorOneMinusDestinationAlpha;
    case 8: return MTLBlendFactorDestinationColor;
    case 9: return MTLBlendFactorOneMinusDestinationColor;
    case 10: return MTLBlendFactorSourceAlphaSaturated;
    case 13: return MTLBlendFactorBlendColor;
    case 14: return MTLBlendFactorOneMinusBlendColor;
    case 15: return MTLBlendFactorSource1Color;
    case 16: return MTLBlendFactorOneMinusSource1Color;
    case 17: return MTLBlendFactorSource1Alpha;
    case 18: return MTLBlendFactorOneMinusSource1Alpha;
    case 19: return MTLBlendFactorBlendAlpha;
    case 20: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}

inline MTLBlendOperation BlendOp(u32 op) {
    switch (op) {
    case 0: return MTLBlendOperationAdd;
    case 1: return MTLBlendOperationSubtract;
    case 2: return MTLBlendOperationMin;
    case 3: return MTLBlendOperationMax;
    case 4: return MTLBlendOperationReverseSubtract;
    default: return MTLBlendOperationAdd;
    }
}

inline MTLCompareFunction CompareFunc(u32 f) {
    switch (f & 7) {
    case 0: return MTLCompareFunctionNever;
    case 1: return MTLCompareFunctionLess;
    case 2: return MTLCompareFunctionEqual;
    case 3: return MTLCompareFunctionLessEqual;
    case 4: return MTLCompareFunctionGreater;
    case 5: return MTLCompareFunctionNotEqual;
    case 6: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

inline MTLSamplerAddressMode AddressMode(u32 m) {
    switch (m & 7) {
    case 0: return MTLSamplerAddressModeRepeat;
    case 1: return MTLSamplerAddressModeMirrorRepeat;
    case 2: return MTLSamplerAddressModeClampToEdge;
    case 3: return MTLSamplerAddressModeMirrorClampToEdge;
    case 4:
    case 6: return MTLSamplerAddressModeClampToBorderColor;
    default: return MTLSamplerAddressModeMirrorClampToEdge;
    }
}

} // namespace Gpu::Metal
