#include "gpu/liverpool/tiling.h"

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

u32 AlignUp(u32 v, u32 a) {
    return (v + a - 1) / a * a;
}

} // namespace

bool IsLinear(u32 tile_index) {
    return tile_index == kLinearAligned || tile_index == kLinearGeneral;
}

bool Supported(u32 tile_index) {
    return IsLinear(tile_index) || MicroType1D(tile_index) != Micro::None;
}

u64 SliceSize(u32 tile_index, u32 pitch_el, u32 height_el, u32 bytes_per_el) {
    if (IsLinear(tile_index)) {
        return u64(pitch_el) * height_el * bytes_per_el;
    }
    return u64(AlignUp(pitch_el, 8)) * AlignUp(height_el, 8) * bytes_per_el;
}

void DetileSlice(u32 tile_index, const u8* src, u8* dst, u32 width_el, u32 height_el, u32 pitch_el,
                 u32 bytes_per_el) {
    const u64 row = u64(width_el) * bytes_per_el;
    if (IsLinear(tile_index)) {
        for (u32 y = 0; y < height_el; ++y) {
            std::memcpy(dst + y * row, src + u64(y) * pitch_el * bytes_per_el, row);
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
