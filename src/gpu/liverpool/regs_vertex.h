// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Gpu::Liverpool {

struct Address {
    u32 address;

    u64 GetAddress() const {
        return static_cast<u64>(address) << 8;
    }
};

enum class IndexType : u32 {
    Index16 = 0,
    Index32 = 1,
};

enum class IndexSwapMode : u32 {
    None = 0,
    Swap16 = 1,
    Swap32 = 2,
    SwapWord = 3,
};

union IndexBufferType {
    u32 raw;
    struct {
        IndexType index_type : 2;
        IndexSwapMode swap_mode : 2;
    };
};

struct PolygonOffset {
    float depth_bias;
    float front_scale;
    float front_offset;
    float back_scale;
    float back_offset;
};

struct TessFactorClamp {
    float hs_max_tess;
    float hs_min_tess;
};

struct IndexBufferBase {
    u32 base_addr_hi : 8;
    u32 base_addr_lo;

    template <typename T = const void*>
    T Address() const {
        return reinterpret_cast<T>((static_cast<u64>(base_addr_lo) & ~1ULL) |
                                   (static_cast<u64>(base_addr_hi) << 32));
    }
};

} // namespace Gpu::Liverpool

