#include "gpu/liverpool/tiling.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace Gpu::Liverpool::Tiling {

namespace {

enum class Micro { Display, Thin, Depth, None };

// Micro tile type of the 1D tile modes in the PS4 tile mode table.
Micro MicroType1D(u32 tile_index) {
    switch (tile_index) {
    case 0x05: return Micro::Depth;   // Depth_1dThin
    case 0x09: return Micro::Display; // Display_1dThin
    case 0x0D: return Micro::Thin;    // Thin_1dThin
    default: return Micro::None;
    }
}

// Index of element (x, y) inside an 8x8 micro tile (x, y in 0..7).
u32 MicroIndex(Micro type, u32 bytes_per_el, u32 x, u32 y) {
    const u32 x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1;
    const u32 y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
    u32 b[6];
    if (type == Micro::Display) {
        switch (bytes_per_el) {
        case 1: b[0] = x0; b[1] = x1; b[2] = x2; b[3] = y1; b[4] = y0; b[5] = y2; break;
        case 2: b[0] = x0; b[1] = x1; b[2] = x2; b[3] = y0; b[4] = y1; b[5] = y2; break;
        case 4: b[0] = x0; b[1] = x1; b[2] = y0; b[3] = x2; b[4] = y1; b[5] = y2; break;
        case 8: b[0] = x0; b[1] = y0; b[2] = x1; b[3] = x2; b[4] = y1; b[5] = y2; break;
        default: b[0] = y0; b[1] = x0; b[2] = x1; b[3] = x2; b[4] = y1; b[5] = y2; break;
        }
    } else { // thin and depth: Morton order
        b[0] = x0; b[1] = y0; b[2] = x1; b[3] = y1; b[4] = x2; b[5] = y2;
    }
    return b[0] | (b[1] << 1) | (b[2] << 2) | (b[3] << 3) | (b[4] << 4) | (b[5] << 5);
}

// Index of element (x, y, z) inside an 8x8x4 thick micro tile.
// Bit layout from shadPS4 tiling.comp ComputePixelIndexWithinMicroTile for
// ADDR_SURF_THIN_MICRO_TILING (which is also used for thick with z bits).
u32 MicroIndex3D(u32 bytes_per_el, u32 x, u32 y, u32 z) {
    const u32 x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1;
    const u32 y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
    const u32 z0 = z & 1, z1 = (z >> 1) & 1;
    u32 p0, p1, p2, p3, p4, p5, p6, p7;
    // Bit ordering depends on bytes per element (from shadPS4 tiling.comp).
    switch (bytes_per_el) {
    case 1:
    case 2:
        p0 = x0; p1 = y0; p2 = x1; p3 = y1; p4 = z0; p5 = z1; break;
    case 4:
        p0 = x0; p1 = y0; p2 = x1; p3 = z0; p4 = y1; p5 = z1; break;
    case 8:
    case 16:
    default:
        p0 = x0; p1 = y0; p2 = z0; p3 = x1; p4 = y1; p5 = z1; break;
    }
    p6 = x2; p7 = y2;
    return p0 | (p1 << 1) | (p2 << 2) | (p3 << 3) | (p4 << 4) | (p5 << 5) | (p6 << 6) | (p7 << 7);
}

// Tile mode 0x13 = Thick_1dThick (Array1DTiledThick), thickness 4.
bool IsThick1D(u32 tile_index) {
    return tile_index == 0x13;
}

u32 AlignUp(u32 v, u32 a) {
    return (v + a - 1) / a * a;
}

u32 Log2(u32 v) {
    return 31u - static_cast<u32>(__builtin_clz(v));
}

// ---- 2D (macro) tiling: 2D_TILED_THIN1 with PS4's P8_32x32_16x16 pipes ----

constexpr u32 kPipes = 8;
constexpr u32 kPipeInterleaveBytes = 256;

Micro MicroType2D(u32 tile_index) {
    switch (tile_index) {
    case 0x00: // Depth_2dThin_64
    case 0x01: // Depth_2dThin_128
    case 0x02: // Depth_2dThin_256
    case 0x03: // Depth_2dThin_512
    case 0x04: // Depth_2dThin_1K
        return Micro::Depth;
    case 0x0A: return Micro::Display; // Display_2dThin
    case 0x0E: return Micro::Thin;    // Thin_2dThin
    default: return Micro::None;
    }
}

struct MacroParams {
    u32 banks;
    u32 bank_height;
    u32 aspect;
    u32 pitch; // macro tile size in elements
    u32 height;
};

// Bank layout for single-sampled colour surfaces; the macro tile mode follows
// from the micro tile size (64 elements x bytes per element).
MacroParams Macro(u32 bytes_per_el) {
    MacroParams m{};
    switch (bytes_per_el) {
    case 1: m = {16, 4, 4}; break;
    case 2: m = {16, 2, 2}; break;
    case 4:
    case 8: m = {16, 1, 2}; break;
    default: m = {8, 1, 1}; break;
    }
    m.pitch = 8 * kPipes * m.aspect;            // bank width is always 1
    m.height = 8 * m.bank_height * m.banks / m.aspect;
    return m;
}

u32 Pipe(u32 x, u32 y) {
    const u32 tx = x / 8, ty = y / 8;
    const u32 x3 = tx & 1, x4 = (tx >> 1) & 1, x5 = (tx >> 2) & 1;
    const u32 y3 = ty & 1, y4 = (ty >> 1) & 1, y5 = (ty >> 2) & 1;
    return (x3 ^ y3 ^ x4) | ((x4 ^ y4) << 1) | ((x5 ^ y5) << 2);
}

u32 Bank(u32 x, u32 y, const MacroParams& m) {
    const u32 tx = x / 8 / kPipes, ty = y / 8 / m.bank_height;
    const u32 x3 = tx & 1, x4 = (tx >> 1) & 1, x5 = (tx >> 2) & 1, x6 = (tx >> 3) & 1;
    const u32 y3 = ty & 1, y4 = (ty >> 1) & 1, y5 = (ty >> 2) & 1, y6 = (ty >> 3) & 1;
    if (m.banks == 16) {
        return (x3 ^ y6) | ((x4 ^ y5 ^ y6) << 1) | ((x5 ^ y4) << 2) | ((x6 ^ y3) << 3);
    }
    return (x3 ^ y5) | ((x4 ^ y4 ^ y5) << 1) | ((x5 ^ y3) << 2); // 8 banks
}

// Byte offset of element (x, y) in a macro-tiled slice.
u64 MacroOffset(u32 x, u32 y, u32 pitch, const MacroParams& m, const u32* micro_order, u32 bpe) {
    const u64 micro_bytes = 64ull * bpe;
    const u64 macro_bytes = micro_bytes * (m.pitch / 8) * (m.height / 8) / (kPipes * m.banks);
    const u64 macro_offset = (u64(y / m.height) * (pitch / m.pitch) + x / m.pitch) * macro_bytes;
    const u64 tile_offset = u64((y / 8) % m.bank_height) * micro_bytes; // bank width 1
    const u64 element = u64(micro_order[(y & 7) * 8 + (x & 7)]) * bpe;
    const u64 total = macro_offset + tile_offset + element;

    const u32 pipe_bits = Log2(kPipes), interleave_bits = Log2(kPipeInterleaveBytes);
    u64 addr = total & (kPipeInterleaveBytes - 1);
    addr |= u64(Pipe(x, y)) << interleave_bits;
    addr |= u64(Bank(x, y, m)) << (interleave_bits + pipe_bits);
    addr |= (total >> interleave_bits) << (interleave_bits + pipe_bits + Log2(m.banks));
    return addr;
}

} // namespace

bool IsLinear(u32 tile_index) {
    return tile_index == kLinearAligned || tile_index == kLinearGeneral;
}

bool Supported(u32 tile_index) {
    return IsLinear(tile_index) || MicroType1D(tile_index) != Micro::None ||
           MicroType2D(tile_index) != Micro::None || IsThick1D(tile_index);
}

u32 Thickness(u32 tile_index) {
    return IsThick1D(tile_index) ? 4 : 1;
}

u64 SliceSize(u32 tile_index, u32 pitch_el, u32 height_el, u32 bytes_per_el) {
    if (IsLinear(tile_index)) {
        return u64(pitch_el) * height_el * bytes_per_el;
    }
    if (MicroType2D(tile_index) != Micro::None) {
        const MacroParams m = Macro(bytes_per_el);
        return u64(AlignUp(pitch_el, m.pitch)) * AlignUp(height_el, m.height) * bytes_per_el;
    }
    const u32 thick = Thickness(tile_index);
    // Thick tiles: each micro tile holds 8x8xthick elements.
    return u64(AlignUp(pitch_el, 8)) * AlignUp(height_el, 8) * bytes_per_el * thick;
}

u64 MipLayout(u32 tile_index, u32 pitch, u32 height, u32 layers, u32 levels, u32 bytes_per_el, bool block,
              bool pow2, MipLevel* out) {
    const u32 thick = Thickness(tile_index);
    const Micro type2d = MicroType2D(tile_index);
    u64 total = 0;
    for (u32 mip = 0; mip < levels; ++mip) {
        u32 w = pitch >> mip, h = height >> mip;
        if (block) {
            w = (w + 3) / 4;
            h = (h + 3) / 4;
        }
        w = std::max(w, 1u);
        h = std::max(h, 1u);
        if (pow2) {
            w = std::bit_ceil(w);
            h = std::bit_ceil(h);
        }
        MipLevel& m = out[mip];
        m.tile_index = tile_index;
        u32 level_thick = 1;
        if (IsLinear(tile_index)) {
            // ImageSizeLinearAligned
            const u32 pitch_align = std::max(8u, 64u / bytes_per_el);
            u32 p = AlignUp(w, pitch_align);
            const u32 slice_align = std::max(64u, 256u / bytes_per_el);
            while ((u64(p) * h) % slice_align) {
                p += pitch_align;
            }
            m.pitch_el = mip == 0 && !block ? std::max(p, pitch) : p;
            m.height_el = h;
        } else if (type2d != Micro::None) {
            const MacroParams mp = Macro(bytes_per_el);
            if (mip == 0 || (w >= mp.pitch && h >= mp.height)) { // IsMacroTiledMip
                m.pitch_el = AlignUp(w, mp.pitch);
                m.height_el = AlignUp(h, mp.height);
            } else { // micro tiled: the 1D mode with the same micro tile type
                m.tile_index = type2d == Micro::Display ? 0x09 : 0x0D;
                m.pitch_el = AlignUp(w, 8);
                m.height_el = AlignUp(h, 8);
                while ((u64(m.pitch_el) * m.height_el * bytes_per_el) % 256) {
                    m.pitch_el += 8;
                }
            }
        } else {
            // ImageSizeMicroTiled
            level_thick = thick;
            m.pitch_el = AlignUp(w, 8);
            m.height_el = AlignUp(h, 8);
            while ((u64(m.pitch_el) * m.height_el * bytes_per_el * level_thick) % 256) {
                m.pitch_el += 8;
            }
        }
        m.slice_size = u64(m.pitch_el) * m.height_el * bytes_per_el * level_thick;
        u32 slices = (layers + level_thick - 1) / level_thick;
        m.offset = total;
        total += m.slice_size * slices;
    }
    return total;
}

void DetileSlice(u32 tile_index, const u8* src, u8* dst, u32 width_el, u32 height_el, u32 pitch_el,
                 u32 bytes_per_el, u32 z) {
    const u64 row = u64(width_el) * bytes_per_el;
    if (IsLinear(tile_index)) {
        for (u32 y = 0; y < height_el; ++y) {
            std::memcpy(dst + y * row, src + u64(y) * pitch_el * bytes_per_el, row);
        }
        return;
    }
    if (const Micro type2d = MicroType2D(tile_index); type2d != Micro::None) {
        const MacroParams m = Macro(bytes_per_el);
        const u32 pitch = AlignUp(pitch_el, m.pitch);
        u32 order[64];
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                order[y * 8 + x] = MicroIndex(type2d, bytes_per_el, x, y);
            }
        }
        for (u32 y = 0; y < height_el; ++y) {
            u8* out = dst + y * row;
            for (u32 x = 0; x < width_el; ++x) {
                std::memcpy(out + u64(x) * bytes_per_el, src + MacroOffset(x, y, pitch, m, order, bytes_per_el),
                            bytes_per_el);
            }
        }
        return;
    }
    if (IsThick1D(tile_index)) {
        // Thick 1D tiling: 8x8x4 micro tiles, thin micro-tile type with z bits.
        const u32 thick = 4;
        const u32 tiles_x = AlignUp(pitch_el, 8) / 8;
        const u64 tile_bytes = u64(8 * 8) * thick * bytes_per_el;
        // Build a lookup table for the 8x8 plane at this z.
        u32 order[64];
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                order[y * 8 + x] = MicroIndex3D(bytes_per_el, x, y, z);
            }
        }
        for (u32 y = 0; y < height_el; ++y) {
            const u8* tile_row = src + u64(y / 8) * tiles_x * tile_bytes;
            u8* out = dst + y * row;
            for (u32 x = 0; x < width_el; ++x) {
                const u8* tile = tile_row + u64(x / 8) * tile_bytes;
                std::memcpy(out + u64(x) * bytes_per_el,
                            tile + u64(order[(y & 7) * 8 + (x & 7)]) * bytes_per_el,
                            bytes_per_el);
            }
        }
        return;
    }
    const Micro type = MicroType1D(tile_index);
    const u32 tiles_x = AlignUp(pitch_el, 8) / 8;
    const u64 tile_bytes = 64ull * bytes_per_el;
    u32 order[64];
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            order[y * 8 + x] = MicroIndex(type, bytes_per_el, x, y);
        }
    }
    for (u32 y = 0; y < height_el; ++y) {
        const u8* tile_row = src + u64(y / 8) * tiles_x * tile_bytes;
        u8* out = dst + y * row;
        for (u32 x = 0; x < width_el; ++x) {
            const u8* tile = tile_row + u64(x / 8) * tile_bytes;
            std::memcpy(out + u64(x) * bytes_per_el, tile + u64(order[(y & 7) * 8 + (x & 7)]) * bytes_per_el,
                        bytes_per_el);
        }
    }
}

} // namespace Gpu::Liverpool::Tiling

