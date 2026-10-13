// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/font/font.h"

#include <cstring>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Font {

namespace {

struct OrbisFontHorizontalLayout {
    float baselineOffset = 14.0f;
    float lineAdvance = 18.0f;
    float decorationExtent = 0.0f;
};

struct OrbisFontGlyphMetrics {
    float width = 16.0f;
    float height = 16.0f;
    struct {
        float bearingX = 0.0f;
        float bearingY = 14.0f;
        float advance = 16.0f;
    } Horizontal;
    struct {
        float bearingX = 0.0f;
        float bearingY = 14.0f;
        float advance = 16.0f;
    } Vertical;
};

alignas(64) static u8 s_dummy_lib[256];
alignas(64) static u8 s_dummy_rend[256];
alignas(64) static u8 s_dummy_font[256];

s32 sceFontMemoryInit(void* mem, void* buf, size_t size, void* alloc, void* free, void* realloc,
                      void* calloc, void* mspace_create, void* mspace_destroy) {
    DC_INFO("sceFontMemoryInit()");
    return 0;
}

s32 sceFontCreateLibraryWithEdition(void* lib_out, void* mem, u32 edition) {
    if (lib_out) {
        *reinterpret_cast<void**>(lib_out) = s_dummy_lib;
    }
    DC_INFO("sceFontCreateLibraryWithEdition()");
    return 0;
}

s32 sceFontCreateRendererWithEdition(void* rend_out, void* mem, u32 edition) {
    if (rend_out) {
        *reinterpret_cast<void**>(rend_out) = s_dummy_rend;
    }
    DC_INFO("sceFontCreateRendererWithEdition()");
    return 0;
}

s32 sceFontAttachDeviceCacheBuffer(void* rend, void* buf, size_t size) {
    return 0;
}

s32 sceFontRenderSurfaceInit(void* surf, void* buf, size_t size, u32 w, u32 h, u32 pitch) {
    return 0;
}

s32 sceFontSupportExternalFonts(void* lib, u32 flags) {
    return 0;
}

s32 sceFontSupportSystemFonts(void* lib, u32 flags) {
    return 0;
}

s32 sceFontSelectLibraryFt(void* lib) {
    return 0;
}

s32 sceFontSelectRendererFt(void* rend) {
    return 0;
}

s32 sceFontOpenFontMemory(void* lib, const void* fontAddress, u32 fontSize,
                          const void* open_params, void** pFontHandle) {
    if (pFontHandle) {
        *pFontHandle = s_dummy_font;
    }
    DC_INFO("sceFontOpenFontMemory(size=%u) -> font handle", fontSize);
    return 0;
}

s32 sceFontSetScalePixel(void* font, float scaleX, float scaleY) {
    return 0;
}

s32 sceFontSetupRenderScalePixel(void* font, float scaleX, float scaleY) {
    return 0;
}

s32 sceFontGetHorizontalLayout(void* font, OrbisFontHorizontalLayout* layout) {
    if (layout) {
        layout->baselineOffset = 14.0f;
        layout->lineAdvance = 18.0f;
        layout->decorationExtent = 0.0f;
    }
    return 0;
}

s32 sceFontGetCharGlyphMetrics(void* font, u32 code, OrbisFontGlyphMetrics* metrics) {
    if (metrics) {
        metrics->width = 16.0f;
        metrics->height = 16.0f;
        metrics->Horizontal.bearingX = 0.0f;
        metrics->Horizontal.bearingY = 14.0f;
        metrics->Horizontal.advance = 16.0f;
        metrics->Vertical.bearingX = 0.0f;
        metrics->Vertical.bearingY = 14.0f;
        metrics->Vertical.advance = 16.0f;
    }
    return 0;
}

s32 sceFontBindRenderer(void* font, void* renderer) {
    return 0;
}

s32 sceFontUnbindRenderer(void* font) {
    return 0;
}

s32 sceFontGetKerning(void* font, u32 code1, u32 code2, void* kerning) {
    if (kerning) {
        std::memset(kerning, 0, 16);
    }
    return 0;
}

s32 sceFontRenderCharGlyphImageHorizontal(void* font, u32 code, void* surf, float x, float y,
                                          OrbisFontGlyphMetrics* metrics, void* result) {
    if (metrics) {
        metrics->width = 16.0f;
        metrics->height = 16.0f;
        metrics->Horizontal.bearingX = 0.0f;
        metrics->Horizontal.bearingY = 14.0f;
        metrics->Horizontal.advance = 16.0f;
    }
    if (result) {
        std::memset(result, 0, 64);
    }
    return 0;
}

} // namespace

void RegisterFont() {
    constexpr const char* kFont = "libSceFont";
    DC_HLE(kFont, sceFontMemoryInit);
    DC_HLE(kFont, sceFontCreateLibraryWithEdition);
    DC_HLE(kFont, sceFontCreateRendererWithEdition);
    DC_HLE(kFont, sceFontAttachDeviceCacheBuffer);
    DC_HLE(kFont, sceFontRenderSurfaceInit);
    DC_HLE(kFont, sceFontSupportExternalFonts);
    DC_HLE(kFont, sceFontSupportSystemFonts);
    DC_HLE(kFont, sceFontOpenFontMemory);
    DC_HLE(kFont, sceFontSetScalePixel);
    DC_HLE(kFont, sceFontSetupRenderScalePixel);
    DC_HLE(kFont, sceFontGetHorizontalLayout);
    DC_HLE(kFont, sceFontGetCharGlyphMetrics);
    DC_HLE(kFont, sceFontBindRenderer);
    DC_HLE(kFont, sceFontUnbindRenderer);
    DC_HLE(kFont, sceFontGetKerning);
    DC_HLE(kFont, sceFontRenderCharGlyphImageHorizontal);

    constexpr const char* kFontFt = "libSceFontFt";
    DC_HLE(kFontFt, sceFontSelectLibraryFt);
    DC_HLE(kFontFt, sceFontSelectRendererFt);
}

} // namespace Hle::Font
