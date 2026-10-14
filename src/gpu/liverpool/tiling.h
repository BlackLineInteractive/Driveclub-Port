#pragma once

// Converts tiled guest surfaces to linear rows.
//
// Liverpool addresses surfaces through a fixed table of tile modes (the T#
// TILING_INDEX). 1D modes split the surface into 8x8-element micro tiles
// stored one after another in row order; the element order inside a micro
// tile depends on the micro tile type. 2D (macro-tiled) modes group micro
// tiles into macro tiles and interleave them across pipes and banks (bit
// layout as in AMD's AddrLib for the PS4's 8-pipe configuration).

#include "common/types.h"

namespace Gpu::Liverpool::Tiling {

constexpr u32 kLinearGeneral = 0x1F;
constexpr u32 kLinearAligned = 0x08;

// True if `tile_index` is linear (no conversion needed).
bool IsLinear(u32 tile_index);

// True if Detile() can handle `tile_index`.
bool Supported(u32 tile_index);

// Micro tile thickness (1 for thin, 4 for thick). Thick modes pack
// multiple z-slices into each 8x8 micro tile.
u32 Thickness(u32 tile_index);

// Bytes of guest memory one slice occupies. For thick modes this is the
// size of one thickness-group (Thickness() slices together).
u64 SliceSize(u32 tile_index, u32 pitch_el, u32 height_el, u32 bytes_per_el);

// Detiles one slice: `src` is guest memory, `dst` gets `width_el` x
// `height_el` elements with rows `width_el * bytes_per_el` apart. An element
// is a texel, or a 4x4 block for block-compressed formats.
// `z` is the z-plane within a thick micro tile (0..Thickness()-1).
void DetileSlice(u32 tile_index, const u8* src, u8* dst, u32 width_el, u32 height_el, u32 pitch_el,
                 u32 bytes_per_el, u32 z = 0);

} // namespace Gpu::Liverpool::Tiling

