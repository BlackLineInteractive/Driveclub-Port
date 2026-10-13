// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/font/font.h"

// libSceFont on CoreText.
//
// The game opens its TrueType/OpenType fonts from memory, sets a pixel
// scale and rasterizes glyphs one at a time into an 8-bit "render surface"
// (its glyph cache), placing each at a pen position on the baseline. CoreText
// does the rasterizing; this file only maps the API onto it.

#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "hle/hle.h"

namespace Hle::Font {

namespace {

constexpr s32 kErrorInvalidParameter = static_cast<s32>(0x80460002);
constexpr s32 kErrorNoSupportGlyph = static_cast<s32>(0x80460010);

struct GlyphMetrics {
    float width;
    float height;
    struct {
        float bearing_x;
        float bearing_y;
        float advance;
    } horizontal, vertical;
};
static_assert(sizeof(GlyphMetrics) == 32);

struct HorizontalLayout {
    float baseline_offset;
    float line_advance;
    float decoration_extent;
};

struct Kerning {
    float offset_x, offset_y, position_x, position_y;
};

struct RenderSurface {
    u8* buffer;
    s32 width_byte;
    s8 pixel_size_byte;
    u8 pad0;
    u8 style_flag;
    u8 pad2;
    s32 width, height;
    u32 sc_x0, sc_y0, sc_x1, sc_y1;
    u64 reserved[11];
};
static_assert(sizeof(RenderSurface) == 128);

struct RenderOutput {
    const void* stage;
    struct {
        u8* address;
        u32 width_byte;
        u8 pixel_size_byte;
        u8 pixel_format;
        u16 pad;
    } surface_image;
    struct {
        u32 x, y, w, h;
    } update_rect;
    struct {
        float bearing_x;
        float bearing_y;
        float advance;
        float stride;
        u32 width;
        u32 height;
    } image_metrics;
};
static_assert(sizeof(RenderOutput) == 64);

struct FontState {
    CTFontRef base = nullptr; // the face at an arbitrary size
    float scale_w = 16.0f, scale_h = 16.0f;
    CTFontRef sized = nullptr; // `base` at the current scale
    float sized_w = 0.0f, sized_h = 0.0f;
};

std::mutex g_lock;
alignas(64) u8 g_library[256];
alignas(64) u8 g_renderer[256];

// The face at `w` x `h` pixels (em size), cached per font.
CTFontRef Sized(FontState& f) {
    if (!f.sized || f.sized_w != f.scale_w || f.sized_h != f.scale_h) {
        if (f.sized) {
            CFRelease(f.sized);
        }
        const CGAffineTransform m = CGAffineTransformMakeScale(f.scale_w / f.scale_h, 1.0);
        f.sized = CTFontCreateCopyWithAttributes(f.base, f.scale_h, &m, nullptr);
        f.sized_w = f.scale_w;
        f.sized_h = f.scale_h;
    }
    return f.sized;
}

bool GlyphFor(CTFontRef font, u32 code, CGGlyph* glyph) {
    UniChar chars[2];
    CFIndex n = 1;
    if (code >= 0x10000) {
        const u32 c = code - 0x10000;
        chars[0] = static_cast<UniChar>(0xD800 + (c >> 10));
        chars[1] = static_cast<UniChar>(0xDC00 + (c & 0x3FF));
        n = 2;
    } else {
        chars[0] = static_cast<UniChar>(code);
    }
    CGGlyph glyphs[2] = {};
    if (!CTFontGetGlyphsForCharacters(font, chars, glyphs, n) || glyphs[0] == 0) {
        return false;
    }
    *glyph = glyphs[0];
    return true;
}

// Metrics in pixels; y up from the baseline, as the API reports them.
void Metrics(CTFontRef font, CGGlyph glyph, GlyphMetrics* m) {
    const CGRect box = CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, nullptr, 1);
    CGSize adv{};
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &adv, 1);
    *m = {};
    m->width = static_cast<float>(box.size.width);
    m->height = static_cast<float>(box.size.height);
    m->horizontal.bearing_x = static_cast<float>(box.origin.x);
    m->horizontal.bearing_y = static_cast<float>(box.origin.y + box.size.height);
    m->horizontal.advance = static_cast<float>(adv.width);
}

s32 sceFontMemoryInit(void* mem, void* buf, u32 size, const void* mem_if, void* object, void* destroy,
                      void* destroy_arg) {
    return 0;
}

s32 sceFontCreateLibraryWithEdition(const void* mem, const void* params, u64 edition, void** lib_out) {
    if (lib_out) {
        *lib_out = g_library;
    }
    return 0;
}

s32 sceFontCreateRendererWithEdition(const void* mem, const void* params, u64 edition, void** rend_out) {
    if (rend_out) {
        *rend_out = g_renderer;
    }
    return 0;
}

s32 sceFontDestroyLibrary(void** lib) {
    return 0;
}

s32 sceFontDestroyRenderer(void** rend) {
    return 0;
}

s32 sceFontAttachDeviceCacheBuffer(void* lib, void* buf, u32 size) {
    return 0;
}

s32 sceFontSupportExternalFonts(void* lib, u32 count, u32 formats) {
    return 0;
}

s32 sceFontSupportSystemFonts(void* lib) {
    return 0;
}

s32 sceFontSelectLibraryFt(u64 value) {
    return 0;
}

s32 sceFontSelectRendererFt(u64 value) {
    return 0;
}

s32 sceFontOpenFontMemory(void* lib, const void* data, u32 size, const void* params, FontState** out) {
    if (!data || !size || !out) {
        return kErrorInvalidParameter;
    }
    CFDataRef bytes = CFDataCreate(nullptr, static_cast<const UInt8*>(data), size);
    CTFontDescriptorRef desc = CTFontManagerCreateFontDescriptorFromData(bytes);
    CFRelease(bytes);
    if (!desc) {
        DC_WARN("font: cannot open a %u-byte font", size);
        return kErrorInvalidParameter;
    }
    auto* f = new FontState;
    f->base = CTFontCreateWithFontDescriptor(desc, 16.0, nullptr);
    CFRelease(desc);
    CFStringRef name = CTFontCopyPostScriptName(f->base);
    char buf[128] = "?";
    if (name) {
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
    }
    DC_INFO("font: opened %s (%u bytes)", buf, size);
    *out = f;
    return 0;
}

s32 sceFontCloseFont(FontState* f) {
    std::scoped_lock lk(g_lock);
    if (f) {
        if (f->sized) {
            CFRelease(f->sized);
        }
        CFRelease(f->base);
        delete f;
    }
    return 0;
}

s32 sceFontBindRenderer(FontState* f, void* renderer) {
    return 0;
}

s32 sceFontUnbindRenderer(FontState* f) {
    return 0;
}

s32 sceFontSetScalePixel(FontState* f, float w, float h) {
    if (!f || w <= 0.0f || h <= 0.0f) {
        return kErrorInvalidParameter;
    }
    std::scoped_lock lk(g_lock);
    f->scale_w = w;
    f->scale_h = h;
    return 0;
}

// The renderer scale is the one glyphs are rasterized at; the game sets
// both to the same value, so one scale serves layout and rendering.
s32 sceFontSetupRenderScalePixel(FontState* f, float w, float h) {
    return sceFontSetScalePixel(f, w, h);
}

s32 sceFontGetHorizontalLayout(FontState* f, HorizontalLayout* layout) {
    if (!f || !layout) {
        return kErrorInvalidParameter;
    }
    std::scoped_lock lk(g_lock);
    CTFontRef font = Sized(*f);
    const float ascent = static_cast<float>(CTFontGetAscent(font));
    const float descent = static_cast<float>(CTFontGetDescent(font));
    layout->baseline_offset = ascent;
    layout->line_advance = ascent + descent + static_cast<float>(CTFontGetLeading(font));
    layout->decoration_extent = static_cast<float>(CTFontGetUnderlineThickness(font));
    return 0;
}

s32 sceFontGetCharGlyphMetrics(FontState* f, u32 code, GlyphMetrics* m) {
    if (!f || !m) {
        return kErrorInvalidParameter;
    }
    std::scoped_lock lk(g_lock);
    CTFontRef font = Sized(*f);
    CGGlyph glyph;
    if (!GlyphFor(font, code, &glyph)) {
        *m = {};
        return kErrorNoSupportGlyph;
    }
    Metrics(font, glyph, m);
    return 0;
}

s32 sceFontGetKerning(FontState* f, u32 pre, u32 code, Kerning* k) {
    if (k) {
        *k = {};
    }
    return 0;
}

void sceFontRenderSurfaceInit(RenderSurface* s, void* buffer, s32 width_byte, s32 pixel_size, s32 width,
                              s32 height) {
    if (!s) {
        return;
    }
    std::memset(s, 0, sizeof(*s));
    s->buffer = static_cast<u8*>(buffer);
    s->width_byte = width_byte;
    s->pixel_size_byte = static_cast<s8>(pixel_size);
    s->width = std::max(width, 0);
    s->height = std::max(height, 0);
    s->sc_x1 = static_cast<u32>(s->width);
    s->sc_y1 = static_cast<u32>(s->height);
}

// Rasterizes `code` with its pen position at (x, y) (y down, on the
// baseline) into the surface, as coverage.
s32 sceFontRenderCharGlyphImageHorizontal(FontState* f, u32 code, RenderSurface* s, float x, float y,
                                          GlyphMetrics* m, RenderOutput* out) {
    if (!f || !s || !m || !out) {
        return kErrorInvalidParameter;
    }
    std::scoped_lock lk(g_lock);
    CTFontRef font = Sized(*f);
    CGGlyph glyph;
    if (!GlyphFor(font, code, &glyph)) {
        *m = {};
        std::memset(out, 0, sizeof(*out));
        return kErrorNoSupportGlyph;
    }
    Metrics(font, glyph, m);

    // Pixel box of the glyph relative to the integer pen position; the
    // fractional part of the pen position shifts the outline.
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const float left_f = fx + m->horizontal.bearing_x;
    const float top_f = m->horizontal.bearing_y - fy; // above the baseline
    const s32 left = static_cast<s32>(std::floor(left_f));
    const s32 right = static_cast<s32>(std::ceil(left_f + m->width));
    const s32 top = static_cast<s32>(std::ceil(top_f));
    const s32 bottom = static_cast<s32>(std::floor(top_f - m->height));
    const s32 gw = std::max(0, right - left), gh = std::max(0, top - bottom);

    const s32 dest_x = static_cast<s32>(std::floor(x)) + left;
    const s32 dest_y = static_cast<s32>(std::floor(y)) - top;
    const s32 bpp = std::max<s32>(1, s->pixel_size_byte);
    s32 ux0 = dest_x, uy0 = dest_y, uw = 0, uh = 0;
    if (gw > 0 && gh > 0) {
        std::vector<u8> pixels(static_cast<size_t>(gw) * gh, 0);
        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        CGContextRef ctx = CGBitmapContextCreate(pixels.data(), gw, gh, 8, gw, gray, kCGImageAlphaNone);
        CGColorSpaceRelease(gray);
        if (ctx) {
            CGContextSetGrayFillColor(ctx, 1.0, 1.0);
            CGContextSetShouldSmoothFonts(ctx, false);
            CGContextSetShouldAntialias(ctx, true);
            const CGPoint origin = CGPointMake(fx - left, -bottom - fy);
            CTFontDrawGlyphs(font, &glyph, &origin, 1, ctx);
            CGContextRelease(ctx);
        }
        const s32 x0 = std::max<s32>(dest_x, s->sc_x0), x1 = std::min<s32>(dest_x + gw, s->sc_x1);
        const s32 y0 = std::max<s32>(dest_y, s->sc_y0), y1 = std::min<s32>(dest_y + gh, s->sc_y1);
        if (x1 > x0 && y1 > y0 && s->buffer) {
            for (s32 row = y0; row < y1; ++row) {
                const u8* src = pixels.data() + static_cast<size_t>(row - dest_y) * gw;
                u8* dst = s->buffer + static_cast<size_t>(row) * s->width_byte;
                for (s32 col = x0; col < x1; ++col) {
                    std::memset(dst + static_cast<size_t>(col) * bpp, src[col - dest_x], bpp);
                }
            }
            ux0 = x0, uy0 = y0, uw = x1 - x0, uh = y1 - y0;
        }
    }

    std::memset(out, 0, sizeof(*out));
    out->update_rect = {static_cast<u32>(std::max(ux0, 0)), static_cast<u32>(std::max(uy0, 0)),
                        static_cast<u32>(uw), static_cast<u32>(uh)};
    out->surface_image.address = s->buffer + static_cast<size_t>(out->update_rect.y) * s->width_byte +
                                 static_cast<size_t>(out->update_rect.x) * bpp;
    out->surface_image.width_byte = static_cast<u32>(s->width_byte);
    out->surface_image.pixel_size_byte = static_cast<u8>(bpp);
    const float advance_end = std::ceil(x + m->horizontal.advance);
    out->image_metrics.bearing_x = static_cast<float>(static_cast<s32>(std::floor(x)) + left) - x;
    out->image_metrics.bearing_y = static_cast<float>(top) + std::floor(y) - y;
    out->image_metrics.advance = advance_end - x;
    out->image_metrics.stride = std::max(advance_end, std::floor(x) + right) - x;
    out->image_metrics.width = static_cast<u32>(gw);
    out->image_metrics.height = static_cast<u32>(gh);
    return 0;
}

s32 sceFontRenderCharGlyphImage(FontState* f, u32 code, RenderSurface* s, float x, float y, GlyphMetrics* m,
                                RenderOutput* out) {
    return sceFontRenderCharGlyphImageHorizontal(f, code, s, x, y, m, out);
}

} // namespace

void RegisterFont() {
    constexpr const char* kFont = "libSceFont";
    DC_HLE(kFont, sceFontMemoryInit);
    DC_HLE(kFont, sceFontCreateLibraryWithEdition);
    DC_HLE(kFont, sceFontCreateRendererWithEdition);
    DC_HLE(kFont, sceFontDestroyLibrary);
    DC_HLE(kFont, sceFontDestroyRenderer);
    DC_HLE(kFont, sceFontAttachDeviceCacheBuffer);
    DC_HLE(kFont, sceFontRenderSurfaceInit);
    DC_HLE(kFont, sceFontSupportExternalFonts);
    DC_HLE(kFont, sceFontSupportSystemFonts);
    DC_HLE(kFont, sceFontOpenFontMemory);
    DC_HLE(kFont, sceFontCloseFont);
    DC_HLE(kFont, sceFontSetScalePixel);
    DC_HLE(kFont, sceFontSetupRenderScalePixel);
    DC_HLE(kFont, sceFontGetHorizontalLayout);
    DC_HLE(kFont, sceFontGetCharGlyphMetrics);
    DC_HLE(kFont, sceFontBindRenderer);
    DC_HLE(kFont, sceFontUnbindRenderer);
    DC_HLE(kFont, sceFontGetKerning);
    DC_HLE(kFont, sceFontRenderCharGlyphImage);
    DC_HLE(kFont, sceFontRenderCharGlyphImageHorizontal);

    constexpr const char* kFontFt = "libSceFontFt";
    DC_HLE(kFontFt, sceFontSelectLibraryFt);
    DC_HLE(kFontFt, sceFontSelectRendererFt);
}

} // namespace Hle::Font
