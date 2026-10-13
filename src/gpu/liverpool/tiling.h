#pragma once

// Converts tiled guest surfaces to linear rows.
//
// Liverpool addresses surfaces through a fixed table of tile modes (the T#
// TILING_INDEX). 1D modes split the surface into 8x8-element micro tiles
// stored one after another in row order; the element order inside a micro
// tile depends on the micro tile type. 2D (macro-tiled) modes add pipe/bank
// swizzling on top and are not handled yet.

#include "common/types.h"

namespace Gpu::Liverpool::Tiling {

constexpr u32 kLinearGeneral = 0x1F;
constexpr u32 kLinearAligned = 0x08;

// True if `tile_index` is linear (no conversion needed).
bool IsLinear(u32 tile_index);

// True if Detile() can handle `tile_index`.
bool Supported(u32 tile_index);

// Bytes of guest memory one slice occupies.
u64 SliceSize(u32 tile_index, u32 pitch_el, u32 height_el, u32 bytes_per_el);

// Detiles one slice: `src` is guest memory, `dst` gets `width_el` x
// `height_el` elements with rows `width_el * bytes_per_el` apart. An element
// is a texel, or a 4x4 block for block-compressed formats.
void DetileSlice(u32 tile_index, const u8* src, u8* dst, u32 width_el, u32 height_el, u32 pitch_el,
                 u32 bytes_per_el);

} // namespace Gpu::Liverpool::Tiling
