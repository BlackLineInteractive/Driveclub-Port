// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "gpu/liverpool/tiling.h"

namespace Gpu::Liverpool {

enum class CompareFunc : u32 {
    Never = 0,
    Less = 1,
    Equal = 2,
    LessEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterEqual = 6,
    Always = 7,
};

struct DepthShaderControl {
    u32 z_export_enable : 1;
    u32 stencil_test_val_export_enable : 1;
    u32 stencil_op_val_export_enable : 1;
    u32 : 1;
    u32 z_order : 2;
    u32 kill_enable : 1;
    u32 coverage_to_mask_enable : 1;
    u32 mask_export_enable : 1;
    u32 exec_on_hier_fail : 1;
    u32 exec_on_noop : 1;
    u32 alpha_to_mask_disable : 1;
    u32 depth_before_shader : 1;
    u32 conservative_z_export : 2;
};

struct DepthControl {
    u32 stencil_enable : 1;
    u32 depth_enable : 1;
    u32 depth_write_enable : 1;
    u32 depth_bounds_enable : 1;
    CompareFunc depth_func : 3;
    u32 backface_enable : 1;
    CompareFunc stencil_ref_func : 3;
    u32 : 9;
    CompareFunc stencil_back_func : 3;
    u32 : 9;
};

struct DepthRenderControl {
    u32 depth_clear_enable : 1;
    u32 stencil_clear_enable : 1;
    u32 depth_compress_disable : 1;
    u32 stencil_compress_disable : 1;
    u32 : 28;
};

struct DepthView {
    u32 slice_start : 11;
    u32 : 2;
    u32 slice_max : 11;
    u32 : 8;
};

struct DepthRenderOverride {
    u32 force_hiz_enable : 2;
    u32 force_his_enable0 : 2;
    u32 force_his_enable1 : 2;
    u32 : 26;
};

struct StencilControl {
    u32 stencil_fail : 3;
    u32 stencil_zpass : 3;
    u32 stencil_zfail : 3;
    u32 stencil_func : 3;
    u32 : 4;
    u32 stencil_back_fail : 3;
    u32 stencil_back_zpass : 3;
    u32 stencil_back_zfail : 3;
    u32 stencil_back_func : 3;
    u32 : 4;
};

struct StencilRefMask {
    u32 stencil_ref : 8;
    u32 stencil_mask : 8;
    u32 stencil_write_mask : 8;
    u32 stencil_op_val : 8;
};

struct DepthBuffer {
    struct {
        u32 z_format : 4;
        u32 : 8;
        u32 num_samples_log2 : 3;
        u32 tile_mode_index : 5;
    } z_info;
    struct {
        u32 stencil_format : 1;
        u32 : 11;
        u32 tile_mode_index : 5;
    } stencil_info;
    u32 z_read_base_address;
    u32 stencil_read_base_address;
    u32 z_write_base_address;
    u32 stencil_write_base_address;
    struct {
        u32 pitch_tile_max : 11;
    } depth_size;
    struct {
        u32 slice_tile_max : 22;
    } depth_slice;

    explicit operator bool() const {
        return z_read_base_address != 0 && z_info.z_format != 0;
    }

    u64 Address() const {
        return static_cast<u64>(z_read_base_address) << 8;
    }
};

} // namespace Gpu::Liverpool
