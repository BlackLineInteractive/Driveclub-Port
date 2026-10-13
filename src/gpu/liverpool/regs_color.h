// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/types.h"
#include "gpu/liverpool/pixel_format.h"
#include "gpu/liverpool/tiling.h"

namespace Gpu::Liverpool {

constexpr u32 NUM_COLOR_BUFFERS = 8;
using BlendConstants = std::array<float, 4>;

struct BlendControl {
    enum class BlendFactor : u32 {
        Zero = 0,
        One = 1,
        SrcColor = 2,
        OneMinusSrcColor = 3,
        SrcAlpha = 4,
        OneMinusSrcAlpha = 5,
        DstAlpha = 6,
        OneMinusDstAlpha = 7,
        DstColor = 8,
        OneMinusDstColor = 9,
        SrcAlphaSaturate = 10,
        ConstantColor = 13,
        OneMinusConstantColor = 14,
        Src1Color = 15,
        InvSrc1Color = 16,
        Src1Alpha = 17,
        InvSrc1Alpha = 18,
        ConstantAlpha = 19,
        OneMinusConstantAlpha = 20,
    };

    enum class BlendFunc : u32 {
        Add = 0,
        Subtract = 1,
        Min = 2,
        Max = 3,
        ReverseSubtract = 4,
    };

    BlendFactor color_src_factor : 5;
    BlendFunc color_func : 3;
    BlendFactor color_dst_factor : 5;
    u32 : 3;
    BlendFactor alpha_src_factor : 5;
    BlendFunc alpha_func : 3;
    BlendFactor alpha_dst_factor : 5;
    u32 separate_alpha_blend : 1;
    u32 enable : 1;
    u32 disable_rop3 : 1;
};

struct ColorControl {
    enum class OperationMode : u32 {
        Disable = 0u,
        Normal = 1u,
        EliminateFastClear = 2u,
        Resolve = 3u,
        Err = 4u,
        FmaskDecompress = 5u,
    };
    enum class LogicOp : u32 {
        Clear = 0x00,
        Copy = 0xCC,
        Set = 0xFF,
    };

    u32 disable_dual_quad : 1;
    u32 : 2;
    u32 degamma_enable : 1;
    OperationMode mode : 3;
    u32 : 9;
    LogicOp rop3 : 8;
};

struct ColorBufferMask {
    u32 mask;
};

struct ColorBuffer {
    u32 base_address;
    struct {
        u32 tile_max : 11;
        u32 : 9;
        u32 fmask_tile_max : 11;
    } pitch;
    struct {
        u32 tile_max : 22;
    } slice;
    struct {
        u32 slice_start : 11;
        u32 : 2;
        u32 slice_max : 11;
    } view;
    union Color0Info {
        u32 raw;
        struct {
            u32 endian : 2;
            u32 format : 5;
            u32 linear_general : 1;
            u32 number_type : 3;
            u32 comp_swap : 2;
            u32 fast_clear : 1;
            u32 compression : 1;
            u32 blend_clamp : 1;
            u32 blend_bypass : 1;
            u32 simple_float : 1;
            u32 round_mode : 1;
            u32 cmask_is_linear : 1;
            u32 blend_opt_dont_rd_dst : 3;
            u32 blend_opt_discard_pixel : 3;
            u32 fmask_compression_disable_ci : 1;
            u32 fmask_compress_1frag_only : 1;
            u32 dcc_enable : 1;
            u32 cmask_addr_type : 2;
            u32 alt_tile_mode : 1;
        };
    } info;
    union Color0Attrib {
        u32 raw;
        struct {
            TileMode tile_mode_index : 5;
            u32 fmask_tile_mode_index : 5;
            u32 fmask_bank_height : 2;
            u32 num_samples_log2 : 3;
            u32 num_fragments_log2 : 2;
            u32 force_dst_alpha_1 : 1;
        };
    } attrib;
    u32 pad0;
    u32 cmask_base_address;
    struct {
        u32 tile_max : 14;
    } cmask_slice;
    u32 fmask_base_address;
    struct {
        u32 tile_max : 14;
    } fmask_slice;
    u32 clear_word0;
    u32 clear_word1;
    std::array<u32, 2> pad1;

    explicit operator bool() const {
        return base_address != 0 && info.format != 0;
    }

    u32 Pitch() const {
        return (pitch.tile_max + 1) << 3;
    }

    u32 Height() const {
        const u32 p = Pitch();
        return p > 0 ? ((slice.tile_max + 1) * 64 / p) : 0;
    }

    u64 Address() const {
        return (static_cast<u64>(base_address) << 8) |
               (info.linear_general ? (view.slice_start & 0xFF) : 0);
    }

    TileMode GetTileMode() const {
        return info.linear_general ? TileMode::DisplayLinearGeneral : attrib.tile_mode_index;
    }

    bool IsTiled() const {
        return GetTileMode() != TileMode::DisplayLinearAligned &&
               GetTileMode() != TileMode::DisplayLinearGeneral;
    }

    DataFormat GetDataFmt() const {
        return static_cast<DataFormat>(info.format);
    }
};

} // namespace Gpu::Liverpool
